# Thread Pool Image Processor

A C++17 multithreaded image processing tool that converts color images to grayscale using a custom thread pool built from scratch.

## What it does

- Implements a **thread pool** from scratch using C++ concurrency primitives (`std::thread`, `std::mutex`, `std::condition_variable`)
- Batch-processes all images in an input directory concurrently
- Converts each image to grayscale using the luminosity formula: `gray = 0.21R + 0.72G + 0.07B`
- Accepts thread count as a CLI argument so you can benchmark any configuration

## Setup

### Prerequisites

- macOS with Xcode Command Line Tools (`xcode-select --install`)
- Or any Linux system with `g++` / `clang++`

### 1. Clone the repo

```bash
git clone https://github.com/Ashmithelps/thread-manager.git
cd thread-manager
```

### 2. Compile

```bash
clang++ -std=c++17 -O2 -Wall -Wextra -pthread -Iinclude -isystem third_party/stb \
  src/pipeline.cpp src/benchmark.cpp apps/cli/main.cpp -o tm-cli
```

### 3. Run

```bash
./tm-cli --input input_images/elisabeth-arnold-oPmIQeKYeqc-unsplash.jpg \
  --threads 1,2,4,8 --tile 32 --blur 4 --runs 5 --out result.jpg --json result.json
```

Run `./tm-cli --help` for all options. The image is converted to grayscale, box-blurred, and benchmarked sequentially and at each thread count. `--out` writes the processed JPEG; `--json` writes the timings plus a per-task trace (worker, phase, tile, start/end in microseconds).

## Demo

| Before | After |
|--------|-------|
| ![Color](assets/demo_before.jpg) | ![Grayscale](assets/demo_after.jpg) |

## Benchmark Results

Tested on **Apple M4** (macOS) — 24 high-resolution JPEG images, total ~104MB.

| Threads | Time    | Speedup |
|---------|---------|---------|
| 1       | 17.57 s | 1x      |
| 8       | 4.21 s  | ~4x     |

**~4x speedup** with 8 threads.

**1 thread:**
![1 thread benchmark](assets/benchmark_1_thread.png)

**8 threads:**
![8 threads benchmark](assets/benchmark_8_threads.png) The bottleneck shifts to disk I/O on large files, which prevents linear scaling — but the concurrent decode/encode pipeline still yields significant gains on real-world workloads.

## Thread Pool Design

```
Main Thread
    │
    ├── enqueue(task_1) ─┐
    ├── enqueue(task_2)  │   ┌─ Worker Thread 1 ── process_image()
    ├── enqueue(task_3)  ├──►│   Worker Thread 2 ── process_image()
    └── enqueue(task_N) ─┘   └─ Worker Thread N ── process_image()
```

- Worker threads block on a condition variable when the queue is empty — zero busy-waiting
- A single `std::mutex` guards the task queue
- The pool destructor sets a stop flag and calls `join()` on all workers — no tasks are dropped mid-run

## Tech Stack

- **Language:** C++17
- **Image I/O:** `stb_image` / `stb_image_write` (header-only, no external dependencies)
- **Concurrency:** `<thread>`, `<mutex>`, `<condition_variable>`, `<functional>`

## Third-Party Code

`stb_image.h` (v2.30) and `stb_image_write.h` (v1.16) are vendored, unmodified, from [nothings/stb](https://github.com/nothings/stb) by Sean Barrett. They are dual-licensed under the MIT License or public domain (Unlicense), at your choice; the full license text is at the end of each header.
