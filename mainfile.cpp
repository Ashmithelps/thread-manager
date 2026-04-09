#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
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
     bool stop; // tells everyone to go to sleep or home

    public:
        ThreadPool(size_t threads);
        void enqueue(std::function<void()> task);
        ~ThreadPool();
};

/*std::vector<std::thread>: Think of this as the Staff List. It’s a container that holds our active chefs.

std::queue: This is a First-In-First-Out (FIFO) list. The first order clipped to the rail is the first one a chef picks up.

std::function<void()>: A Wrapper. It’s like a generic "Job Description." It doesn't matter if the job is "Wash Dishes" or "Chop Onions," this wrapper can hold it as long as it’s a task that can be executed.*/


ThreadPool::ThreadPool(size_t num_threads) : stop(false){
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
                task();
            }
            
        });
    }
}


void ThreadPool::enqueue(std::function<void()>task){
    {
        std::unique_lock<std::mutex>lock(queue_mutex);
        tasks.push(task);
    }
    condition.notify_one();
}



ThreadPool::~ThreadPool(){
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop=true;
    }
    condition.notify_all();
    for(std::thread &worker : workers){
        worker.join();
    }
}

void process_image(std::string input_path, std::string output_path){
    int width, height, channels;
    //load the image into memory 
    unsigned char* img=stbi_load(input_path.c_str(), &width, &height, &channels,0);
    if(img==NULL)return;

    for(int i=0;i<width*height*channels;i+=channels){
        unsigned char gray=(unsigned char)(0.21*img[i] + 0.72*img[i+1] + 0.07*img[i+2]);
        img[i]=gray;
        img[i+1]=gray;
        img[i+2]=gray;
    }
    stbi_write_jpg(output_path.c_str(), width, height, channels, img, 100);
    stbi_image_free(img);
}



int main() {
    // 1. Setup paths (Make sure these folders exist on your Mac!)
    std::string input_dir = "input_images";
    std::string output_dir = "output_images";

    // Create the output folder if it doesn't exist
    if (!fs::exists(output_dir)) {
        fs::create_directory(output_dir);
    }

    // 2. Start the Stopwatch
    auto start = std::chrono::high_resolution_clock::now();

    {
        // 3. Hire your chefs (using 8 since you have a powerful M4 chip!)
        ThreadPool pool(1);

        // 4. Scan the 'input_images' folder for work
        for (const auto& entry : fs::directory_iterator(input_dir)) {
            if (entry.is_regular_file()) {
                std::string input_path = entry.path().string();
                std::string output_path = output_dir + "/" + entry.path().filename().string();

                // Send the "Grayscale Soup" order to the chefs
                pool.enqueue([input_path, output_path] {
                    process_image(input_path, output_path);
                });
            }
        }
        
        // The pool's destructor (at the end of this '{ }' block) 
        // will make the main program wait until all chefs are done.
    }

    // 5. Stop the Stopwatch
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    std::cout << "Successfully processed all images in: " << diff.count() << " seconds!" << std::endl;

    return 0;
}