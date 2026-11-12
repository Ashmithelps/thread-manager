#include "tm/pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <stdexcept>
#include <string>

// The library never touches the filesystem: compile stb without its file APIs so that's enforced.
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace tmgr {

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t micros_since(Clock::time_point t0, Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::microseconds>(t - t0).count();
}

// --- kernels: each one processes rows [y0, y1) and writes only those rows of dst ---

void grayscale_rows(const std::uint8_t* rgb, std::uint8_t* gray, int width, int y0, int y1) {
    for (int y = y0; y < y1; ++y) {
        const std::uint8_t* src = rgb + static_cast<std::size_t>(y) * width * 3;
        std::uint8_t* dst = gray + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            dst[x] = static_cast<std::uint8_t>(0.21 * src[3 * x] + 0.72 * src[3 * x + 1] + 0.07 * src[3 * x + 2]);
        }
    }
}

// box blur along each row; reads only rows [y0, y1) of src
void blur_horizontal_rows(const std::uint8_t* src, std::uint8_t* dst, int width, int radius, int y0, int y1) {
    const unsigned window = 2 * static_cast<unsigned>(radius) + 1;
    for (int y = y0; y < y1; ++y) {
        const std::uint8_t* in = src + static_cast<std::size_t>(y) * width;
        std::uint8_t* out = dst + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            unsigned sum = 0;
            for (int k = -radius; k <= radius; ++k) {
                sum += in[std::clamp(x + k, 0, width - 1)];
            }
            out[x] = static_cast<std::uint8_t>((sum + window / 2) / window);
        }
    }
}

// box blur down each column; reads rows [y0 - radius, y1 + radius) of src, i.e. other tiles' rows
void blur_vertical_rows(const std::uint8_t* src, std::uint8_t* dst, int width, int height, int radius, int y0, int y1) {
    const unsigned window = 2 * static_cast<unsigned>(radius) + 1;
    thread_local std::vector<unsigned> sums; // reused per thread, so tasks don't allocate
    sums.resize(static_cast<std::size_t>(width));
    for (int y = y0; y < y1; ++y) {
        std::fill(sums.begin(), sums.end(), 0u);
        // accumulate whole rows so memory is read sequentially
        for (int k = -radius; k <= radius; ++k) {
            const std::uint8_t* in = src + static_cast<std::size_t>(std::clamp(y + k, 0, height - 1)) * width;
            for (int x = 0; x < width; ++x) sums[x] += in[x];
        }
        std::uint8_t* out = dst + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            out[x] = static_cast<std::uint8_t>((sums[x] + window / 2) / window);
        }
    }
}

// Waits for every future before rethrowing the first error, so no task is still
// writing into the pipeline's buffers when an exception leaves this function.
void wait_all(std::vector<std::future<void>>& futures) {
    for (auto& f : futures) {
        if (f.valid()) f.wait();
    }
    for (auto& f : futures) {
        if (f.valid()) f.get();
    }
    futures.clear();
}

// Submits one task per tile and waits for all of them (a barrier between phases).
// Each task writes only its own trace slot, so tracing needs no lock; future.get()
// makes those writes visible to the calling thread.
template <class Kernel>
void run_phase(ThreadPool& pool, std::vector<std::future<void>>& pending, Phase phase,
               int tile_count, int tile_height, int height, Clock::time_point t0,
               TaskTrace* slots, Kernel kernel) {
    pending.clear();
    try {
        for (int tile = 0; tile < tile_count; ++tile) {
            TaskTrace* slot = slots + tile;
            pending.push_back(pool.submit([=] {
                const auto start = Clock::now();
                const int y0 = tile * tile_height;
                const int y1 = std::min(height, y0 + tile_height);
                kernel(y0, y1);
                const auto end = Clock::now();
                *slot = TaskTrace{ThreadPool::current_worker_index(), phase, tile,
                                  micros_since(t0, start), micros_since(t0, end)};
            }));
        }
    } catch (...) {
        // submit failed part-way: tasks already queued still reference our buffers
        for (auto& f : pending) f.wait();
        pending.clear();
        throw;
    }
    wait_all(pending);
}

} // namespace

const char* phase_name(Phase phase) {
    switch (phase) {
        case Phase::Grayscale: return "grayscale";
        case Phase::BlurHorizontal: return "blur_horizontal";
        case Phase::BlurVertical: return "blur_vertical";
    }
    return "unknown";
}

Image decode_image(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) throw std::runtime_error("image data is empty");
    if (size > static_cast<std::size_t>(INT_MAX)) throw std::runtime_error("image data is larger than 2 GB");
    const int len = static_cast<int>(size);

    // check dimensions from the header first, so an oversized image is rejected before allocating it
    int width = 0, height = 0, channels_in_file = 0;
    if (!stbi_info_from_memory(data, len, &width, &height, &channels_in_file)) {
        throw std::runtime_error(std::string("failed to read image header: ") + stbi_failure_reason());
    }
    if (width > kMaxImageDimension || height > kMaxImageDimension) {
        throw std::runtime_error("image is " + std::to_string(width) + "x" + std::to_string(height) +
                                 "; the maximum is " + std::to_string(kMaxImageDimension) + "x" +
                                 std::to_string(kMaxImageDimension));
    }

    constexpr int channels = 3; // force RGB so grayscale and grayscale+alpha inputs are safe to index
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> decoded(
        stbi_load_from_memory(data, len, &width, &height, &channels_in_file, channels),
        &stbi_image_free);
    if (!decoded) throw std::runtime_error(std::string("failed to decode image: ") + stbi_failure_reason());

    Image image;
    image.width = width;
    image.height = height;
    image.channels = channels;
    const std::size_t num_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * channels;
    image.pixels.assign(decoded.get(), decoded.get() + num_bytes);
    return image;
}

std::vector<std::uint8_t> encode_jpeg(const Image& image, int quality) {
    if (image.channels != 1 && image.channels != 3) throw std::runtime_error("encode_jpeg needs a 1- or 3-channel image");
    if (image.pixels.size() != static_cast<std::size_t>(image.width) * image.height * image.channels) {
        throw std::runtime_error("encode_jpeg: pixel buffer size doesn't match the image dimensions");
    }
    std::vector<std::uint8_t> bytes;
    auto append = [](void* context, void* data, int size) {
        auto* out = static_cast<std::vector<std::uint8_t>*>(context);
        const auto* begin = static_cast<const std::uint8_t*>(data);
        out->insert(out->end(), begin, begin + size);
    };
    if (!stbi_write_jpg_to_func(append, &bytes, image.width, image.height, image.channels,
                                image.pixels.data(), quality)) {
        throw std::runtime_error("failed to encode JPEG");
    }
    return bytes;
}

GrayscaleBlurPipeline::GrayscaleBlurPipeline(const Image& rgb, PipelineConfig config)
    : rgb_(&rgb), config_(config) {
    if (rgb.channels != 3) throw std::invalid_argument("pipeline input must be 3-channel RGB");
    if (rgb.width <= 0 || rgb.height <= 0) throw std::invalid_argument("pipeline input is empty");
    if (rgb.width > kMaxImageDimension || rgb.height > kMaxImageDimension) {
        throw std::invalid_argument("pipeline input exceeds " + std::to_string(kMaxImageDimension) + " pixels per side");
    }
    if (rgb.pixels.size() != static_cast<std::size_t>(rgb.width) * rgb.height * 3) {
        throw std::invalid_argument("pipeline input pixel buffer size doesn't match its dimensions");
    }
    if (config.tile_height < 1) throw std::invalid_argument("tile height must be at least 1");
    if (config.blur_radius < 0 || config.blur_radius > kMaxBlurRadius) {
        throw std::invalid_argument("blur radius must be between 0 and " + std::to_string(kMaxBlurRadius));
    }

    config_.tile_height = std::min(config.tile_height, rgb.height);
    tile_count_ = (rgb.height + config_.tile_height - 1) / config_.tile_height;

    const std::size_t plane = static_cast<std::size_t>(rgb.width) * rgb.height;
    gray_.resize(plane);
    if (config_.blur_radius > 0) {
        tmp_.resize(plane);
        out_.resize(plane);
    }
    trace_.resize(static_cast<std::size_t>(phase_count()) * tile_count_);
    pending_.reserve(static_cast<std::size_t>(tile_count_));
}

void GrayscaleBlurPipeline::run_sequential() {
    const int w = rgb_->width, h = rgb_->height, r = config_.blur_radius;
    grayscale_rows(rgb_->pixels.data(), gray_.data(), w, 0, h);
    if (r > 0) {
        blur_horizontal_rows(gray_.data(), tmp_.data(), w, r, 0, h);
        blur_vertical_rows(tmp_.data(), out_.data(), w, h, r, 0, h);
    }
}

void GrayscaleBlurPipeline::run_parallel(ThreadPool& pool) {
    const int w = rgb_->width, h = rgb_->height, r = config_.blur_radius;
    const int th = config_.tile_height, tiles = tile_count_;
    const std::uint8_t* rgb = rgb_->pixels.data();
    std::uint8_t* gray = gray_.data();
    std::uint8_t* tmp = tmp_.data();
    std::uint8_t* out = out_.data();
    const auto t0 = Clock::now();

    // Every phase reads one buffer and writes another, and each task writes only its own rows,
    // so tasks within a phase never write overlapping memory.
    run_phase(pool, pending_, Phase::Grayscale, tiles, th, h, t0, &trace_[0],
              [=](int y0, int y1) { grayscale_rows(rgb, gray, w, y0, y1); });

    if (r == 0) return;

    // run_phase returned only after every grayscale tile finished. That barrier matters because
    // the blur reads neighbouring rows: a tile near a band edge needs rows that belong to other
    // tiles, and without the barrier it could read rows that haven't been converted yet.
    // (The horizontal pass on its own reads only its own rows; the vertical pass is the one that
    // reaches up to `radius` rows into the tiles above and below, so it must wait for the whole
    // horizontal pass.)
    run_phase(pool, pending_, Phase::BlurHorizontal, tiles, th, h, t0, &trace_[tiles],
              [=](int y0, int y1) { blur_horizontal_rows(gray, tmp, w, r, y0, y1); });

    run_phase(pool, pending_, Phase::BlurVertical, tiles, th, h, t0, &trace_[2 * static_cast<std::size_t>(tiles)],
              [=](int y0, int y1) { blur_vertical_rows(tmp, out, w, h, r, y0, y1); });
}

Image GrayscaleBlurPipeline::result() const {
    Image image;
    image.width = rgb_->width;
    image.height = rgb_->height;
    image.channels = 1;
    image.pixels = config_.blur_radius > 0 ? out_ : gray_;
    return image;
}

} // namespace tmgr
