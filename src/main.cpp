#ifdef _HAS_CUDA_
#include <cuda_runtime.h>
#endif
#include <gflags/gflags.h>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>
#include "SCAMP.h"
#include "SCAMP.pb.h"
#include "common.h"

DEFINE_int32(num_cpu_workers, 0, "Number of CPU workers to use");
DEFINE_bool(output_pearson, false,
            "If true SCAMP will output pearson correlation instead of "
            "z-normalized euclidean distance.");
DEFINE_bool(
    no_gpu, false,
    "If true SCAMP will not use any GPUs to compute the matrix profile");
DEFINE_int32(max_tile_size, 1 << 20, "Maximum tile size SCAMP will use");
DEFINE_string(window, "", "Length of subsequences to search for");
DEFINE_double(
    threshold, std::nan("NaN"),
    "Distance threshold for frequency and sum calculations, we will only count "
    "events with a Pearson correlation above this threshold.");
DEFINE_string(
    profile_type, "1NN_INDEX",
    "Matrix Profile Type to compute, must be one of \"1NN_INDEX, SUM_THRESH\", "
    "1NN_INDEX generates the classic Matrix Profile, SUM_THRESH generates a "
    "sum of the correlations above threshold set by the --threshold flag.");
DEFINE_bool(double_precision, false, "Computation in double precision");
DEFINE_bool(mixed_precision, false, "Computation in mixed precision");
DEFINE_bool(single_precision, false, "Computation in single precision");
DEFINE_bool(
    keep_rows, false,
    "Informs SCAMP to compute the \"rowwise mp\" and output in a a separate "
    "file specified by the flag --output_b_file_name, only valid for ab-joins, "
    "this is useful when computing a distributed self-join, so as to not "
    "recompute values in the lower-trianglular portion of the symmetric "
    "distance matrix.");
DEFINE_bool(aligned, false,
            "For ab-joins which are partially self-joins. And for distributed "
            "self-joins. Indicates that A and B may start with the same "
            "sequence and must consider an exclusion zone");
DEFINE_int64(
    global_row, -1,
    "Informs SCAMP that this join is part of a larger distributed join which "
    "starts at this row in the larger distance matrix, this allows us to pick "
    "an appropriate exclusion zone for our computation if necessary.");
DEFINE_int64(
    global_col, -1,
    "Informs SCAMP that this join is part of a larger distributed join which "
    "starts at this column in the larger distance matrix, this allows us to "
    "pick an appropriate exclusion zone for our computation if necessary");
DEFINE_string(input_a_file_name, "",
              "Primary input file name for a self-join or ab-join");
DEFINE_string(input_b_file_name, "",
              "Secondary input file name for an ab-join");
DEFINE_string(output_a_file_name, "mp_columns_out",
              "Primary output file name for the matrix profile \"columns\"");
DEFINE_string(
    output_a_index_file_name, "mp_columns_out_index",
    "Primary output file name for the matrix profile \"columns\" index (if "
    "ab-join these are indexes into input_b) this flag is only used when "
    "generating a matrix profile which contains an index");
DEFINE_string(output_b_file_name, "mp_rows_out",
              "Output the matrix profile for the \"rows\" as a separate file "
              "with this name");
DEFINE_string(
    output_b_index_file_name, "mp_rows_out_index",
    "Primary output file name for the matrix profile \"columns\" index (if "
    "ab-join these are indexes into input_a) this flag is only used when "
    "generating a matrix profile which contains an index");
DEFINE_string(gpus, "",
              "IDs of GPUs on the system to use, if this flag is not set SCAMP "
              "tries to use all available GPUs on the system");

// Reads input time series from file
template <class DTYPE>
void readFile(const std::string &filename, std::vector<DTYPE> &v,
              const char *format_str) {
  std::ifstream f(filename);
  if (f.fail()) {
    std::cout << "Unable to open" << filename
              << "for reading, please make sure it exists" << std::endl;
    exit(0);
  }
  DTYPE num;
  while (f >> num) {
    v.push_back(num);
  }
  std::cout << "Read " << v.size() << " values from file " << filename << std::endl;
}

std::vector<int> ParseIntList(const std::string &s) {
  // TODO(zpzim): check regex for formatting
  if (s.empty()) {
    return std::vector<int>();
  }
  std::stringstream ss(s);
  std::vector<int> result;
  while (ss.good()) {
    std::string substr;
    std::getline(ss, substr, ',');
    result.push_back(std::stoi(substr));
  }
  return result;
}

SCAMP::SCAMPPrecisionType GetPrecisionType(bool doublep, bool mixedp,
                                           bool singlep) {
  if (doublep) {
    return SCAMP::PRECISION_DOUBLE;
  }
  if (mixedp) {
    return SCAMP::PRECISION_MIXED;
  }
  if (singlep) {
    return SCAMP::PRECISION_SINGLE;
  }
  return SCAMP::PRECISION_INVALID;
}

SCAMP::SCAMPProfileType ParseProfileType(const std::string &s) {
  if (s == "1NN_INDEX") {
    return SCAMP::PROFILE_TYPE_1NN_INDEX;
  }
  if (s == "SUM_THRESH") {
    return SCAMP::PROFILE_TYPE_SUM_THRESH;
  }
  if (s == "1NN") {
    return SCAMP::PROFILE_TYPE_1NN;
  }
  return SCAMP::PROFILE_TYPE_INVALID;
}

template <typename T>
T ConvertToEuclidean(T val, int window) {
  return std::sqrt(std::max(2.0 * window * (1.0 - val), 0.0));
}

void MergeWithResult(SCAMP::Profile *result, SCAMP::Profile *to_merge) {
  switch(to_merge->type()) {
    case SCAMP::PROFILE_TYPE_1NN_INDEX:
      result->mutable_data()
            ->Add()
            ->mutable_uint64_value()
            ->mutable_value()
            ->Swap(to_merge->mutable_data()->Mutable(0)->mutable_uint64_value()->mutable_value());
      break;
    case SCAMP::PROFILE_TYPE_1NN:
      result->mutable_data()
            ->Add()
            ->mutable_float_value()
            ->mutable_value()
            ->Swap(to_merge->mutable_data()->Mutable(0)->mutable_float_value()->mutable_value());
      break;
    case SCAMP::PROFILE_TYPE_SUM_THRESH:
      result->mutable_data()
            ->Add()
            ->mutable_double_value()
            ->mutable_value()
            ->Swap(to_merge->mutable_data()->Mutable(0)->mutable_double_value()->mutable_value());
      break;
    default:
      break;
    }
}

bool WriteProfileToFile(const std::string &mp, const std::string &mpi,
                        SCAMP::Profile p, std::vector<int> windows, int pad_to_size) { 
  switch (p.type()) {
    case SCAMP::PROFILE_TYPE_1NN_INDEX: {
      std::ofstream mp_out(mp);
      std::ofstream mpi_out(mpi);
      std::cout << p.data().size() << std::endl;
      for (int x = 0; x < p.data().size(); ++x) {
        auto arr = p.data().Get(x).uint64_value().value();
        for (int i = 0; i < pad_to_size; ++i) {
          if ( i < arr.size()) {
            SCAMP::mp_entry e;
            e.ulong = arr.Get(i);
            if (FLAGS_output_pearson) {
              mp_out << std::setprecision(10) << e.floats[0] << " ";
            } else {
              mp_out << std::setprecision(10)
                     << ConvertToEuclidean<float>(e.floats[0], windows[x]) << " ";
            }
            mpi_out << e.ints[1] + 1 << " ";
          } else {
            mp_out << "NaN ";
            mpi_out << "NaN ";
          }
          }
        mp_out << std::endl;
        mpi_out << std::endl;
      }
      break;
    }
    case SCAMP::PROFILE_TYPE_1NN: {
      std::ofstream mp_out(mp);
      for (int x = 0; x < p.data().size(); ++x) {
        auto arr = p.data().Get(0).float_value().value();
        for (int i = 0; i < arr.size(); ++i) {
          if (FLAGS_output_pearson) {
            mp_out << std::setprecision(10) << arr.Get(i) << " ";
          } else {
            mp_out << std::setprecision(10) << ConvertToEuclidean<float>(arr.Get(i), windows[x]) 
                   << " ";
          }
        }
        mp_out << std::endl;
      }
      break;
    }
    case SCAMP::PROFILE_TYPE_SUM_THRESH: {
      std::ofstream mp_out(mp);
      for (int x = 0; x < p.data().size(); ++x) {
        auto arr = p.data().Get(x).double_value().value();
        for (int i = 0; i < arr.size(); ++i) {
          if( i < arr.size() ) {
            SCAMP::mp_entry e;
            e.ulong = arr.Get(i);
            mp_out << std::setprecision(10) << arr.Get(i) << " ";
          }else {
            mp_out << "NaN ";
          }
        }
        mp_out << std::endl;
      }
      break;
    }
    default:
      break;
    }
  return true;
}

void InitProfileMemory(SCAMP::SCAMPArgs *args) {
  switch (args->profile_type()) {
    case SCAMP::PROFILE_TYPE_1NN_INDEX: {
      SCAMP::mp_entry e;
      e.floats[0] = std::numeric_limits<float>::lowest();
      e.ints[1] = -1u;
      vector<uint64_t> temp(args->timeseries_a().size() - args->window() + 1,
                            e.ulong);
      {
        google::protobuf::RepeatedField<uint64_t> data(temp.begin(),
                                                       temp.end());
        args->mutable_profile_a()
            ->mutable_data()
            ->Add()
            ->mutable_uint64_value()
            ->mutable_value()
            ->Swap(&data);
      }
      if (args->keep_rows_separate()) {
        temp.resize(args->timeseries_b().size() - args->window() + 1, e.ulong);
        google::protobuf::RepeatedField<uint64_t> data(temp.begin(),
                                                       temp.end());
        args->mutable_profile_b()
            ->mutable_data()
            ->Add()
            ->mutable_uint64_value()
            ->mutable_value()
            ->Swap(&data);
      }
    }
    case SCAMP::PROFILE_TYPE_1NN: {
      vector<float> temp(args->timeseries_a().size() - args->window() + 1,
                         std::numeric_limits<float>::lowest());
      {
        google::protobuf::RepeatedField<float> data(temp.begin(), temp.end());
        args->mutable_profile_a()
            ->mutable_data()
            ->Add()
            ->mutable_float_value()
            ->mutable_value()
            ->Swap(&data);
      }
      if (args->keep_rows_separate()) {
        temp.resize(args->timeseries_b().size() - args->window() + 1,
                    std::numeric_limits<float>::lowest());
        google::protobuf::RepeatedField<float> data(temp.begin(), temp.end());
        args->mutable_profile_b()
            ->mutable_data()
            ->Add()
            ->mutable_float_value()
            ->mutable_value()
            ->Swap(&data);
      }
    }
    case SCAMP::PROFILE_TYPE_SUM_THRESH: {
      vector<double> temp(args->timeseries_a().size() - args->window() + 1, 0);
      {
        google::protobuf::RepeatedField<double> data(temp.begin(), temp.end());
        args->mutable_profile_a()
            ->mutable_data()
            ->Add()
            ->mutable_double_value()
            ->mutable_value()
            ->Swap(&data);
      }
      if (args->keep_rows_separate()) {
        temp.resize(args->timeseries_b().size() - args->window() + 1, 0);
        google::protobuf::RepeatedField<double> data(temp.begin(), temp.end());
        args->mutable_profile_b()
            ->mutable_data()
            ->Add()
            ->mutable_double_value()
            ->mutable_value()
            ->Swap(&data);
      }
      break;
    }
    default:
      break;
  }
}

int main(int argc, char **argv) {
  bool self_join, computing_rows, computing_cols;
  size_t start_row = 0;
  size_t start_col = 0;
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  if (!FLAGS_double_precision && !FLAGS_mixed_precision &&
      !FLAGS_single_precision) {
    FLAGS_double_precision = true;
  }
  if ((FLAGS_double_precision ? 1 : 0) + (FLAGS_mixed_precision ? 1 : 0) +
          (FLAGS_single_precision ? 1 : 0) !=
      1) {
    printf("Error: only one precision flag can be enabled at a time\n");
    return 1;
  }
  std::vector<int> windows = ParseIntList(FLAGS_window);
  if (windows.empty()) {
    printf(
        "--window=<window_size> to specify your subsequence length.\n");
    return 1;
  }
  if (FLAGS_max_tile_size < 1024) {
    printf("Error: max tile size must be at least 1024\n");
    return 1;
  }
  for (const auto &window: windows) {
    if(window < 3) { 
      std::cout << "Error: Subsequence lengths must be at least 3, got window size of " << window;
      return 1;
    }
    if (FLAGS_max_tile_size / 2 < window) {
      printf(
        "Error: Tile length and width must be at least 2x larger than the "
        "window size. Please set a larger --max_tile_size=<max_tile_size>\n");
      return 1;
    }
  }
  std::vector<int> devices = ParseIntList(FLAGS_gpus);
  if (FLAGS_input_a_file_name.empty()) {
    printf(
        "Error: primary input filename must be specified using "
        "--input_a_file_name");
    return 1;
  }
  if (FLAGS_input_b_file_name.empty()) {
    self_join = true;
    computing_rows = true;
    computing_cols = true;
  } else {
    self_join = false;
    computing_cols = true;
    computing_rows = FLAGS_keep_rows;
  }
  SCAMP::SCAMPPrecisionType t = GetPrecisionType(
      FLAGS_double_precision, FLAGS_mixed_precision, FLAGS_single_precision);
  SCAMP::SCAMPProfileType profile_type = ParseProfileType(FLAGS_profile_type);

  std::vector<double> Ta_h, Tb_h;

  readFile<double>(FLAGS_input_a_file_name, Ta_h, "%lf");

  if (!self_join) {
    readFile<double>(FLAGS_input_b_file_name, Tb_h, "%lf");
  }

#ifdef _HAS_CUDA_
  if (devices.empty() && !FLAGS_no_gpu) {
    // Use all available devices
    printf("using all devices\n");
    int num_dev;
    cudaGetDeviceCount(&num_dev);
    for (int i = 0; i < num_dev; ++i) {
      devices.push_back(i);
    }
  }
#else
  // We cannot use gpus if we don't have CUDA
  assert(devices.empty() &&
         "This binary was not built with CUDA, --gpus cannot be used with this "
         "binary.");
#endif
  SCAMP::SCAMPArgs args;
  args.set_max_tile_size(FLAGS_max_tile_size);
  args.set_has_b(!self_join);
  args.set_distributed_start_row(FLAGS_global_row);
  args.set_distributed_start_col(FLAGS_global_col);
  args.set_distance_threshold(static_cast<double>(FLAGS_threshold));
  args.set_computing_columns(computing_cols);
  args.set_computing_rows(computing_rows);
  args.mutable_profile_a()->set_type(profile_type);
  args.mutable_profile_b()->set_type(profile_type);
  args.set_precision_type(t);
  args.set_profile_type(profile_type);
  args.set_keep_rows_separate(FLAGS_keep_rows);
  args.set_is_aligned(FLAGS_aligned);
  printf("precision = %d\n", args.precision_type());
  {
    google::protobuf::RepeatedField<double> data(Ta_h.begin(), Ta_h.end());
    args.mutable_timeseries_a()->Swap(&data);
    data = google::protobuf::RepeatedField<double>(Tb_h.begin(), Tb_h.end());
    args.mutable_timeseries_b()->Swap(&data);
  }
  SCAMP::Profile result_a, result_b;
  result_a.set_type(profile_type);
  result_b.set_type(profile_type);
  printf("Starting SCAMP\n");
  for (const auto& window : windows) {
    std::cout << "Window: " << window << std::endl;
    args.set_window(window);
    InitProfileMemory(&args);
    SCAMP::do_SCAMP(&args, devices, FLAGS_num_cpu_workers);
    MergeWithResult(&result_a, args.mutable_profile_a());
    if (FLAGS_keep_rows){
      MergeWithResult(&result_b, args.mutable_profile_b());
    }
    args.mutable_profile_a()->mutable_data()->Clear();
    args.mutable_profile_b()->mutable_data()->Clear();
  }
  printf("Now writing result to files\n");
  WriteProfileToFile(FLAGS_output_a_file_name, FLAGS_output_a_index_file_name,
                     result_a, windows, Ta_h.size() - windows[0] + 1);
  if (FLAGS_keep_rows) {
    WriteProfileToFile(FLAGS_output_b_file_name, FLAGS_output_b_index_file_name,
                       result_b, windows, Tb_h.size() - windows[0] + 1);
  }
#ifdef _HAS_CUDA_
  gpuErrchk(cudaDeviceSynchronize());
  gpuErrchk(cudaDeviceReset());
#endif
  printf("Done\n");
  return 0;
}
