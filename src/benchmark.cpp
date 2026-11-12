#include "tm/benchmark.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <stdexcept>
#include <thread>

namespace tmgr {

namespace {

using Clock = std::chrono::steady_clock;

template <class Run>
double time_ms(Run&& run) {
    const auto start = Clock::now();
    run();
    const auto end = Clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

RunStats summarize(std::vector<double> samples) {
    RunStats stats;
    stats.samples_ms = samples;
    const std::size_t n = samples.size();

    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return samples[a] < samples[b]; });

    stats.min_ms = samples[order.front()];
    stats.median_run = order[(n - 1) / 2];
    stats.median_ms = n % 2 == 1 ? samples[order[n / 2]]
                                 : (samples[order[n / 2 - 1]] + samples[order[n / 2]]) / 2;

    if (n > 1) {
        const double mean = std::accumulate(samples.begin(), samples.end(), 0.0) / n;
        double sq = 0;
        for (double s : samples) sq += (s - mean) * (s - mean);
        stats.stddev_ms = std::sqrt(sq / (n - 1));
    }
    return stats;
}

// Minimal JSON writer: tracks whether a comma is needed at each nesting level.
class JsonWriter {
public:
    JsonWriter& begin_object() { prefix(); out_ += '{'; first_.push_back(true); return *this; }
    JsonWriter& end_object() { first_.pop_back(); out_ += '}'; return *this; }
    JsonWriter& begin_array() { prefix(); out_ += '['; first_.push_back(true); return *this; }
    JsonWriter& end_array() { first_.pop_back(); out_ += ']'; return *this; }

    JsonWriter& key(const std::string& name) {
        prefix();
        write_string(name);
        out_ += ':';
        after_key_ = true;
        return *this;
    }

    JsonWriter& value(const std::string& s) { prefix(); write_string(s); return *this; }
    JsonWriter& value(const char* s) { return value(std::string(s)); }
    JsonWriter& value(bool b) { prefix(); out_ += b ? "true" : "false"; return *this; }
    JsonWriter& value(long long n) { prefix(); out_ += std::to_string(n); return *this; }
    JsonWriter& value(int n) { return value(static_cast<long long>(n)); }
    JsonWriter& value(unsigned n) { return value(static_cast<long long>(n)); }
    JsonWriter& value(std::size_t n) { return value(static_cast<long long>(n)); }
    JsonWriter& value(double d) {
        prefix();
        if (!std::isfinite(d)) { out_ += "null"; return *this; }
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.3f", d);
        out_ += buf;
        return *this;
    }

    template <class T>
    JsonWriter& field(const std::string& name, const T& v) { key(name); return value(v); }

    const std::string& str() const { return out_; }

private:
    void prefix() {
        if (after_key_) { after_key_ = false; return; }
        if (!first_.empty()) {
            if (!first_.back()) out_ += ',';
            first_.back() = false;
        }
    }

    void write_string(const std::string& s) {
        out_ += '"';
        for (char c : s) {
            switch (c) {
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n"; break;
                case '\t': out_ += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof buf, "\\u%04x", c);
                        out_ += buf;
                    } else {
                        out_ += c;
                    }
            }
        }
        out_ += '"';
    }

    std::string out_;
    std::vector<bool> first_;
    bool after_key_ = false;
};

void write_stats(JsonWriter& json, const RunStats& stats) {
    json.field("median_ms", stats.median_ms)
        .field("min_ms", stats.min_ms)
        .field("stddev_ms", stats.stddev_ms);
    json.key("samples_ms").begin_array();
    for (double s : stats.samples_ms) json.value(s);
    json.end_array();
}

} // namespace

BenchmarkResult run_benchmark(const Image& rgb, const BenchmarkConfig& config) {
    if (config.thread_counts.empty()) throw std::invalid_argument("benchmark needs at least one thread count");
    if (config.runs_per_config < 1) throw std::invalid_argument("runs per config must be at least 1");
    if (config.warmup_runs < 0) throw std::invalid_argument("warmup runs can't be negative");

    // validates the config and allocates every buffer before anything is timed
    GrayscaleBlurPipeline pipeline(rgb, PipelineConfig{config.tile_height, config.blur_radius});

    BenchmarkResult result;
    result.width = rgb.width;
    result.height = rgb.height;
    result.tile_count = pipeline.tile_count();
    result.hardware_concurrency = std::thread::hardware_concurrency();
    result.config = config;

    // sequential baseline: same kernels over the whole image, no pool, no futures
    for (int i = 0; i < config.warmup_runs; ++i) pipeline.run_sequential();
    std::vector<double> samples;
    for (int i = 0; i < config.runs_per_config; ++i) {
        samples.push_back(time_ms([&] { pipeline.run_sequential(); }));
    }
    result.sequential = summarize(samples);
    result.output = pipeline.result();

    for (std::size_t threads : config.thread_counts) {
        ThreadPool pool(threads); // created outside the timed region

        for (int i = 0; i < config.warmup_runs; ++i) pipeline.run_parallel(pool);

        samples.clear();
        std::vector<std::vector<TaskTrace>> traces;
        for (int i = 0; i < config.runs_per_config; ++i) {
            samples.push_back(time_ms([&] { pipeline.run_parallel(pool); }));
            traces.push_back(pipeline.trace());
        }

        ThreadCountResult r;
        r.threads = threads;
        r.stats = summarize(samples);
        r.speedup = result.sequential.median_ms / r.stats.median_ms;
        r.output_matches_sequential = pipeline.result().pixels == result.output.pixels;
        r.trace = std::move(traces[r.stats.median_run]);
        result.results.push_back(std::move(r));
    }
    return result;
}

std::string to_json(const BenchmarkResult& result) {
    JsonWriter json;
    json.begin_object();

    json.key("image").begin_object()
        .field("width", result.width)
        .field("height", result.height)
        .end_object();

    json.key("config").begin_object();
    json.key("thread_counts").begin_array();
    for (std::size_t t : result.config.thread_counts) json.value(t);
    json.end_array();
    json.field("tile_height", result.config.tile_height)
        .field("tile_count", result.tile_count)
        .field("blur_radius", result.config.blur_radius)
        .field("runs_per_config", result.config.runs_per_config)
        .field("warmup_runs", result.config.warmup_runs)
        .end_object();

    json.field("hardware_concurrency", result.hardware_concurrency);
    json.field("timing_note", "Times cover only the processing phases. Decoding, encoding, buffer "
                              "allocation and thread pool creation are excluded.");

    json.key("sequential").begin_object();
    write_stats(json, result.sequential);
    json.end_object();

    json.key("results").begin_array();
    for (const ThreadCountResult& r : result.results) {
        json.begin_object().field("threads", r.threads);
        write_stats(json, r.stats);
        json.field("speedup", r.speedup)
            .field("output_matches_sequential", r.output_matches_sequential);
        json.key("trace").begin_array();
        for (const TaskTrace& t : r.trace) {
            json.begin_object()
                .field("worker", t.worker_id)
                .field("phase", phase_name(t.phase))
                .field("tile", t.tile_index)
                .field("start_us", static_cast<long long>(t.start_us))
                .field("end_us", static_cast<long long>(t.end_us))
                .end_object();
        }
        json.end_array();
        json.end_object();
    }
    json.end_array();

    json.end_object();
    return json.str() + "\n";
}

} // namespace tmgr
