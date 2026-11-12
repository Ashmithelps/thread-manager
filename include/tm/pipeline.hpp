#pragma once

#include <cstddef>
#include <cstdint>
#include <future>
#include <vector>

#include "tm/thread_pool.hpp"

namespace tmgr {

constexpr int kMaxImageDimension = 6000;
constexpr int kMaxBlurRadius = 50;

// 8-bit interleaved pixels, row-major, no padding between rows
struct Image {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<std::uint8_t> pixels;
};

// Decodes an encoded image (JPEG, PNG, BMP, ...) held in memory into 3-channel RGB.
// Throws std::runtime_error if the data can't be decoded or either side exceeds kMaxImageDimension.
Image decode_image(const std::uint8_t* data, std::size_t size);

// Encodes a 1- or 3-channel image as JPEG in memory. Throws std::runtime_error on failure.
std::vector<std::uint8_t> encode_jpeg(const Image& image, int quality = 90);

enum class Phase : std::uint8_t { Grayscale, BlurHorizontal, BlurVertical };
const char* phase_name(Phase phase);

// One task's execution record. Times are microseconds since the start of the run.
struct TaskTrace {
    int worker_id = -1;
    Phase phase = Phase::Grayscale;
    int tile_index = 0;
    std::int64_t start_us = 0;
    std::int64_t end_us = 0;
};

struct PipelineConfig {
    int tile_height = 32; // rows per task
    int blur_radius = 0;  // 0 = grayscale only, max kMaxBlurRadius
};

// Grayscale followed by a separable box blur, split into horizontal row bands ("tiles").
// Buffers are allocated once in the constructor so repeated runs measure only the processing.
// The input image must outlive the pipeline.
class GrayscaleBlurPipeline {
public:
    GrayscaleBlurPipeline(const Image& rgb, PipelineConfig config);

    // whole image on the calling thread, no pool and no tracing
    void run_sequential();

    // one task per tile per phase; fills trace()
    void run_parallel(ThreadPool& pool);

    // 1-channel grayscale (and blurred, if blur_radius > 0) output of the last run
    Image result() const;

    // trace of the last run_parallel(), indexed by phase * tile_count() + tile
    const std::vector<TaskTrace>& trace() const { return trace_; }

    int tile_count() const { return tile_count_; }
    int phase_count() const { return config_.blur_radius > 0 ? 3 : 1; }

private:
    const Image* rgb_;
    PipelineConfig config_;
    int tile_count_ = 0;
    std::vector<std::uint8_t> gray_; // phase 1 output
    std::vector<std::uint8_t> tmp_;  // horizontal blur output
    std::vector<std::uint8_t> out_;  // vertical blur output
    std::vector<TaskTrace> trace_;
    std::vector<std::future<void>> pending_;
};

} // namespace tmgr
