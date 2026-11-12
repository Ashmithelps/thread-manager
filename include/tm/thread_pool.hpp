#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

// "tmgr" rather than "tm": a namespace named tm would clash with struct ::tm from <ctime>
namespace tmgr {

class ThreadPool{
    private:
     std::vector<std::thread>workers; // the chefs or say workers
     std::queue<std::function<void()>>tasks; // the order line and function void is created because it can hold any piece of code


     // to keep things in sync
     std::mutex queue_mutex ;// the rule of one lock , means only one chef can accept one order
     std::condition_variable condition; // the kitchen bell
     bool stop; // tells everyone to go to sleep or home, guarded by queue_mutex
     std::once_flag shutdown_once; // closing time happens exactly once

     // each chef's badge number (0..N-1), set once when the chef starts; -1 on any thread that isn't a chef
     static inline thread_local int worker_index = -1;

    public:
        explicit ThreadPool(size_t threads);
        ~ThreadPool();

        // a pool owns live threads, so it can't be copied or moved
        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;
        ThreadPool(ThreadPool&&) = delete;
        ThreadPool& operator=(ThreadPool&&) = delete;

        // place an order and get a ticket (future) to collect the result or the exception.
        // arguments are copied, like std::thread and std::async; wrap them in std::ref to pass by reference
        template <class F, class... Args>
        std::future<std::invoke_result_t<F, Args...>> submit(F&& f, Args&&... args);

        // stop taking orders, finish the queued ones, then send everyone home.
        // must not be called from inside a task (a worker can't join itself)
        void shutdown();

        size_t size() const { return workers.size(); }

        // badge number of the calling worker in its pool, or -1 if the caller isn't a pool worker
        static int current_worker_index() { return worker_index; }
};

/*std::vector<std::thread>: Think of this as the Staff List. It’s a container that holds our active chefs.

std::queue: This is a First-In-First-Out (FIFO) list. The first order clipped to the rail is the first one a chef picks up.

std::function<void()>: A Wrapper. It’s like a generic "Job Description." It doesn't matter if the job is "Wash Dishes" or "Chop Onions," this wrapper can hold it as long as it’s a task that can be executed.

std::packaged_task: The order plus its ticket. Whatever the job returns, or whatever it throws, gets stored in the ticket (std::future) for the customer to collect with get().*/


inline ThreadPool::ThreadPool(size_t num_threads) : stop(false){
    if(num_threads==0) throw std::invalid_argument("ThreadPool needs at least one thread");

    workers.reserve(num_threads);
    try{
        for(size_t i=0;i<num_threads;++i){
            workers.emplace_back([this, i]{
                worker_index=static_cast<int>(i);
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


inline void ThreadPool::shutdown(){
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


inline ThreadPool::~ThreadPool(){
    shutdown();
}

} // namespace tmgr
