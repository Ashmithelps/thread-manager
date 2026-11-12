// Native command-line front end: does all file I/O, then hands bytes to the library.

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "tm/benchmark.hpp"

namespace fs = std::filesystem;

namespace {

struct Options {
    std::string input;
    std::string out;  // optional .jpg
    std::string json; // optional
    tmgr::BenchmarkConfig bench;
};

size_t max_threads() {
    return 4 * static_cast<size_t>(std::max(1u, std::thread::hardware_concurrency()));
}

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " --input <image> [options]\n"
              << "  --threads <list>  comma-separated thread counts, each 1.." << max_threads()
              << " (4 x hardware threads). Default: 1,2,4,8\n"
              << "  --tile <rows>     rows per task, >= 1. Default: 32\n"
              << "  --blur <radius>   box blur radius, 0.." << tmgr::kMaxBlurRadius << " (0 = no blur). Default: 4\n"
              << "  --runs <n>        timed runs per thread count, 1..1000. Default: 5\n"
              << "  --warmup <n>      untimed runs before timing, 0..100. Default: 1\n"
              << "  --out <file.jpg>  write the processed image (JPEG)\n"
              << "  --json <file>     write results and per-task traces as JSON\n";
}

// accepts only a plain decimal number in [lo, hi]
template <class T>
bool parse_number(const std::string& text, T lo, T hi, T& out) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc() && ptr == end && out >= lo && out <= hi;
}

bool parse_thread_list(const std::string& text, std::vector<size_t>& out) {
    out.clear();
    size_t pos = 0;
    while (true) {
        const size_t comma = text.find(',', pos);
        size_t n = 0;
        if (!parse_number(text.substr(pos, comma - pos), size_t{1}, max_threads(), n)) return false;
        out.push_back(n);
        if (comma == std::string::npos) return true;
        pos = comma + 1;
    }
}

bool has_jpeg_extension(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".jpg" || ext == ".jpeg";
}

// returns false (after printing why) if the arguments are invalid
bool parse_args(int argc, char* argv[], Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--help" || flag == "-h") return false;
        if (i + 1 >= argc) {
            std::cerr << "Error: " << flag << " needs a value\n";
            return false;
        }
        const std::string value = argv[++i];
        bool ok = true;
        if (flag == "--input") opt.input = value;
        else if (flag == "--out") opt.out = value;
        else if (flag == "--json") opt.json = value;
        else if (flag == "--threads") ok = parse_thread_list(value, opt.bench.thread_counts);
        else if (flag == "--tile") ok = parse_number(value, 1, tmgr::kMaxImageDimension, opt.bench.tile_height);
        else if (flag == "--blur") ok = parse_number(value, 0, tmgr::kMaxBlurRadius, opt.bench.blur_radius);
        else if (flag == "--runs") ok = parse_number(value, 1, 1000, opt.bench.runs_per_config);
        else if (flag == "--warmup") ok = parse_number(value, 0, 100, opt.bench.warmup_runs);
        else {
            std::cerr << "Error: unknown option " << flag << "\n";
            return false;
        }
        if (!ok) {
            std::cerr << "Error: invalid value '" << value << "' for " << flag << "\n";
            return false;
        }
    }
    if (opt.input.empty()) {
        std::cerr << "Error: --input is required\n";
        return false;
    }
    if (!opt.out.empty() && !has_jpeg_extension(opt.out)) {
        std::cerr << "Error: --out must end in .jpg or .jpeg (output is always JPEG)\n";
        return false;
    }
    return true;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    if (!fs::is_regular_file(path)) throw std::runtime_error("input file '" + path + "' not found");
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't open '" + path + "'");
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) throw std::runtime_error("error reading '" + path + "'");
    return bytes;
}

void write_file(const std::string& path, const void* data, size_t size) {
    std::ofstream out(path, std::ios::binary);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    out.close();
    if (!out) throw std::runtime_error("failed to write '" + path + "'");
}

void print_table(const tmgr::BenchmarkResult& r) {
    std::printf("\n%-8s %12s %10s %12s %9s  %s\n", "threads", "median ms", "min ms", "stddev ms", "speedup", "output");
    std::printf("%-8s %12.2f %10.2f %12.2f %9s  %s\n", "seq", r.sequential.median_ms, r.sequential.min_ms,
                r.sequential.stddev_ms, "1.00x", "reference");
    for (const auto& t : r.results) {
        std::printf("%-8zu %12.2f %10.2f %12.2f %8.2fx  %s\n", t.threads, t.stats.median_ms, t.stats.min_ms,
                    t.stats.stddev_ms, t.speedup, t.output_matches_sequential ? "matches" : "MISMATCH");
    }
}

} // namespace

int main(int argc, char* argv[]) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        print_usage(argv[0]);
        return 1;
    }

    try {
        const std::vector<std::uint8_t> bytes = read_file(opt.input);
        const tmgr::Image rgb = tmgr::decode_image(bytes.data(), bytes.size()); // decoded once, before timing

        std::cout << "Image: " << opt.input << " (" << rgb.width << "x" << rgb.height << ")\n"
                  << "Tile height: " << opt.bench.tile_height << " rows, blur radius: " << opt.bench.blur_radius
                  << ", runs: " << opt.bench.runs_per_config << " (+" << opt.bench.warmup_runs << " warmup)\n"
                  << "Hardware threads: " << std::thread::hardware_concurrency() << "\n"
                  << "Timing covers the processing phases only; decoding, encoding and thread pool creation are excluded.\n";

        const tmgr::BenchmarkResult result = tmgr::run_benchmark(rgb, opt.bench);
        std::cout << "Tiles per phase: " << result.tile_count << "\n";
        print_table(result);

        if (!opt.out.empty()) {
            const std::vector<std::uint8_t> jpeg = tmgr::encode_jpeg(result.output);
            write_file(opt.out, jpeg.data(), jpeg.size());
            std::cout << "\nWrote " << opt.out << "\n";
        }
        if (!opt.json.empty()) {
            const std::string json = tmgr::to_json(result);
            write_file(opt.json, json.data(), json.size());
            std::cout << "Wrote " << opt.json << "\n";
        }

        const bool all_match = std::all_of(result.results.begin(), result.results.end(),
                                           [](const auto& t) { return t.output_matches_sequential; });
        if (!all_match) {
            std::cerr << "Error: a parallel run produced different pixels from the sequential run\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
