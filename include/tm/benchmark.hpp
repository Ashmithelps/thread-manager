#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "tm/pipeline.hpp"

namespace tmgr {

struct BenchmarkConfig {
    std::vector<std::size_t> thread_counts{1, 2, 4, 8};
    int tile_height = 32;
    int blur_radius = 4;
    int runs_per_config = 5; // timed runs per thread count
    int warmup_runs = 1;     // untimed runs before the timed ones
};

struct RunStats {
    double median_ms = 0;
    double min_ms = 0;
    double stddev_ms = 0;            // sample standard deviation
    std::vector<double> samples_ms;  // in run order
    std::size_t median_run = 0;      // index into samples_ms of the run reported as the median
};

struct ThreadCountResult {
    std::size_t threads = 0;
    RunStats stats;
    double speedup = 0;              // sequential median / this median
    bool output_matches_sequential = false;
    std::vector<TaskTrace> trace;    // trace of the median run
};

struct BenchmarkResult {
    int width = 0;
    int height = 0;
    int tile_count = 0;
    unsigned hardware_concurrency = 0;
    BenchmarkConfig config;
    RunStats sequential;
    std::vector<ThreadCountResult> results;
    Image output; // from the sequential run
};

// Times the sequential baseline and then each thread count. Only the processing phases are
// timed: the image is already decoded, buffers are allocated up front, and each thread pool
// is created before its runs start.
BenchmarkResult run_benchmark(const Image& rgb, const BenchmarkConfig& config);

std::string to_json(const BenchmarkResult& result);

} // namespace tmgr
