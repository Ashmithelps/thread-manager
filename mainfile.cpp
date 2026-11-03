#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <tuple>
#include <type_traits>
#include <stdexcept>
#include <atomic>
#include <string>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <filesystem>
#include <chrono>

namespace fs=std::filesystem;
class ThreadPool{
    private:
     std::vector<std::thread>workers; // the chefs or say workers
     std::queue<std::function<void()>>tasks; // the order line and function void is created because it can hold any piece of code


     // to keep things in sync
     std::mutex queue_mutex ;// the rule of one lock , means only one chef can accept one order
     std::condition_variable condition; // the kitchen bell
     bool stop; // tells everyone to go to sleep or home, guarded by queue_mutex
     std::once_flag shutdown_once; // closing time happens exactly once

    public:
        explicit ThreadPool(size_t threads);
        ~ThreadPool();

        // a pool owns live threads, so it can't be copied or moved
        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;
        ThreadPool(ThreadPool&&) = delete;
        ThreadPool& operator=(ThreadPool&&) = delete;

        // place an order and get a ticket (future) to collect the result or the exception
        template <class F, class... Args>
        std::future<std::invoke_result_t<F, Args...>> submit(F&& f, Args&&... args);

        // stop taking orders, finish the queued ones, then send everyone home
        void shutdown();
};

/*std::vector<std::thread>: Think of this as the Staff List. It’s a container that holds our active chefs.

std::queue: This is a First-In-First-Out (FIFO) list. The first order clipped to the rail is the first one a chef picks up.

std::function<void()>: A Wrapper. It’s like a generic "Job Description." It doesn't matter if the job is "Wash Dishes" or "Chop Onions," this wrapper can hold it as long as it’s a task that can be executed.

std::packaged_task: The order plus its ticket. Whatever the job returns, or whatever it throws, gets stored in the ticket (std::future) for the customer to collect with get().*/


ThreadPool::ThreadPool(size_t num_threads) : stop(false){
    if(num_threads==0) throw std::invalid_argument("ThreadPool needs at least one thread");

    workers.reserve(num_threads);
    try{
        for(size_t i=0;i<num_threads;++i){
            workers.emplace_back([this]{
                while (true)
                {
                    std::function<void()>task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this]{
                            return this->stop || !this->tasks.empty();
                        });
                        if(this->stop && this->tasks.empty()) return;

                        task=std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task(); // never throws: packaged_task stores exceptions in the future
                }

            });
        }
    }catch(...){
        // a chef failed to show up: send home the ones already hired, then report the error
        // (the destructor won't run for a half-built object, so this is the only chance to join them)
        shutdown();
        throw;
    }
}


template <class F, class... Args>
std::future<std::invoke_result_t<F, Args...>> ThreadPool::submit(F&& f, Args&&... args){
    using R = std::invoke_result_t<F, Args...>;

    // std::function needs a copyable callable but packaged_task is move-only,
    // so the task lives in a shared_ptr and the queue holds a copyable handle to it
    auto task = std::make_shared<std::packaged_task<R()>>(
        [f = std::forward<F>(f), args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            return std::apply(std::move(f), std::move(args));
        });
    std::future<R> result = task->get_future();

    {
        std::unique_lock<std::mutex>lock(queue_mutex);
        if(stop) throw std::runtime_error("submit() called on a ThreadPool that has been shut down");
        tasks.emplace([task]{ (*task)(); });
    }
    condition.notify_one();
    return result;
}


void ThreadPool::shutdown(){
    std::call_once(shutdown_once, [this]{
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop=true;
        }
        condition.notify_all();
        for(std::thread &worker : workers){
            worker.join();
        }
    });
}


ThreadPool::~ThreadPool(){
    shutdown();
}

// lowercase file extension including the dot, e.g. ".jpg"
std::string lower_extension(const fs::path& path){
    std::string ext=path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return ext;
}

bool is_supported_image(const fs::path& path){
    const std::string ext=lower_extension(path);
    return ext==".jpg" || ext==".jpeg" || ext==".png" || ext==".bmp" || ext==".tga";
}

// throws std::runtime_error on failure so the error reaches main through the future
void process_image(const std::string& input_path, const std::string& output_path){
    constexpr int channels=3; // force RGB so grayscale and grayscale+alpha inputs are safe to index
    int width, height, channels_in_file;
    //load the image into memory, freed automatically even if we throw
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> img(
        stbi_load(input_path.c_str(), &width, &height, &channels_in_file, channels),
        &stbi_image_free);
    if(!img) throw std::runtime_error("failed to load " + input_path + ": " + stbi_failure_reason());

    unsigned char* px=img.get();
    const size_t num_bytes=static_cast<size_t>(width) * static_cast<size_t>(height) * channels;
    for(size_t i=0;i<num_bytes;i+=channels){
        unsigned char gray=(unsigned char)(0.21*px[i] + 0.72*px[i+1] + 0.07*px[i+2]);
        px[i]=gray;
        px[i+1]=gray;
        px[i+2]=gray;
    }

    // write in the format the file extension promises
    const std::string ext=lower_extension(output_path);
    int written=0;
    if(ext==".png")      written=stbi_write_png(output_path.c_str(), width, height, channels, px, width*channels);
    else if(ext==".bmp") written=stbi_write_bmp(output_path.c_str(), width, height, channels, px);
    else if(ext==".tga") written=stbi_write_tga(output_path.c_str(), width, height, channels, px);
    else                 written=stbi_write_jpg(output_path.c_str(), width, height, channels, px, 100);
    if(!written) throw std::runtime_error("failed to write " + output_path);
}

// accepts only a plain decimal number in [1, max_threads]
bool parse_thread_count(const char* text, size_t max_threads, size_t& out){
    const char* end=text + std::strlen(text);
    auto [ptr, ec]=std::from_chars(text, end, out);
    return ec==std::errc() && ptr==end && out>=1 && out<=max_threads;
}



int main(int argc, char* argv[]) {
    std::string input_dir = "input_images";
    std::string output_dir = "output_images";

    const size_t max_threads = 4 * static_cast<size_t>(std::max(1u, std::thread::hardware_concurrency()));
    size_t num_threads = 1;
    if (argc > 2 || (argc == 2 && !parse_thread_count(argv[1], max_threads, num_threads))) {
        std::cerr << "Usage: " << argv[0] << " [num_threads]\n"
                  << "  num_threads must be a whole number from 1 to " << max_threads
                  << " (4 x hardware threads). Default: 1\n";
        return 1;
    }

    if (!fs::is_directory(input_dir)) {
        std::cerr << "Error: input directory '" << input_dir << "' not found. "
                  << "Run from the repo root, or create it and add images.\n";
        return 1;
    }

    if (!fs::exists(output_dir)) {
        fs::create_directory(output_dir);
    }

    std::cout << "Processing with " << num_threads << " thread(s)..." << std::endl;

    auto start = std::chrono::steady_clock::now();

    std::atomic<size_t> succeeded{0};
    std::atomic<size_t> failed{0};
    {
        ThreadPool pool(num_threads);
        std::vector<std::future<void>> results;

        // 4. Scan the 'input_images' folder for work
        for (const auto& entry : fs::directory_iterator(input_dir)) {
            if (entry.is_regular_file() && is_supported_image(entry.path())) {
                std::string input_path = entry.path().string();
                std::string output_path = output_dir + "/" + entry.path().filename().string();

                // Send the "Grayscale Soup" order to the chefs
                results.push_back(pool.submit([input_path, output_path, &succeeded, &failed] {
                    try {
                        process_image(input_path, output_path);
                        succeeded.fetch_add(1, std::memory_order_relaxed);
                    } catch (...) {
                        failed.fetch_add(1, std::memory_order_relaxed);
                        throw; // let the caller see why through future.get()
                    }
                }));
            }
        }

        // collect every ticket; get() rethrows anything the task threw
        for (auto& result : results) {
            try {
                result.get();
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n";
            }
        }

        // The pool's destructor (at the end of this '{ }' block)
        // calls shutdown(), which joins all the chefs.
    }

    // 5. Stop the Stopwatch
    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double> diff = end - start;

    if (failed == 0 && succeeded > 0) {
        std::cout << "Successfully processed all " << succeeded << " images in: " << diff.count() << " seconds!" << std::endl;
        return 0;
    }
    std::cout << "Processed " << succeeded << " image(s), " << failed << " failed, in: " << diff.count() << " seconds." << std::endl;
    return failed == 0 ? 0 : 1;
}
