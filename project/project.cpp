#include <iostream>
#include <chrono>
#include <thread>
#include <random>
#include <unordered_set>
#include <vector>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <utility>

constexpr bool MY_OWN_SORT = true; // Set to true to use custom sort, false to use std::sort

// Lomuto partition scheme
int32_t MyPartition(std::vector<double>& vec, int32_t lo, int32_t hi) {
    double pivot = vec[hi];
    int32_t i = lo;
    for (int32_t j = lo; j <= hi - 1; j++) {
        if (vec[j] <= pivot) {
            std::swap(vec[i], vec[j]);
			i++;
        }
    }
	std::swap(vec[i], vec[hi]);
	return i;
}

// iterative implementation of quicksort
std::vector<std::pair<int32_t, int32_t>> stack;

void MyQsortIterative(std::vector<double>& vec, int32_t lo, int32_t hi) {
	if ((lo >= hi) || (lo < 0)) return; // not necessary, but just in case
    stack.push_back({ lo, hi });

    while (!stack.empty()) {
        auto [current_lo, current_hi] = stack.back(); stack.pop_back();
        if (current_lo >= current_hi) continue;
        int32_t p = MyPartition(vec, current_lo, current_hi);
        stack.push_back({ p + 1, current_hi });
        stack.push_back({ current_lo, p - 1 });
    }
}

// recursive implementation of quicksort
void MyQsortRecursive(std::vector<double>& vec, int32_t lo, int32_t hi) {
	if ((lo >= hi) || (lo < 0)) return;
	int32_t p = MyPartition(vec, lo, hi);
    MyQsortRecursive(vec, lo, p - 1);
    MyQsortRecursive(vec, p + 1, hi);
}

// Multithreaded Iterative QuickSort
void MyQsortParallelIterative(std::vector<double>& vec, int32_t lo, int32_t hi) {
    if ((lo >= hi) || (lo < 0)) return;

	int32_t size_threshold = 1000; // Threshold for switching to single-threaded quicksort
    if (hi - lo < size_threshold) {
		std::cout << "Array size is small (less than "<< size_threshold <<"), using single - threaded MyQsort..." << std::endl;
        MyQsortIterative(vec, lo, hi);
        return;
    }

    // Core count (8 for my case)
    unsigned int num_cores = std::thread::hardware_concurrency();
    std::cout << "Detected " << num_cores << " hardware threads." << std::endl;
    if (num_cores == 0) num_cores = 1;

    struct Range {
        int32_t lo;
        int32_t hi;
    };
    std::queue<Range> task_queue;

    std::mutex queue_mutex;
    std::condition_variable cv;

    // Tracking of active tasks
    std::atomic<int32_t> active_tasks{ 0 };
    std::atomic<bool> shutdown{ false };

    // Push the initial main range
    task_queue.push({ lo, hi });
    active_tasks++;

    // Worker thread function
    auto worker_func = [&](int thread_id) {
        (void)thread_id; // Unused
        while (true) {
            Range current_range{ -1, -1 };

            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                cv.wait(lock, [&]() {
                    return !task_queue.empty() || shutdown.load();
                    });

                if (shutdown.load()) {
                    break;
                }

                if (!task_queue.empty()) {
                    current_range = task_queue.front();
                    task_queue.pop();
                }
                else {
                    continue;
                }
            }

            if (current_range.lo < current_range.hi) {
                int32_t p = MyPartition(vec, current_range.lo, current_range.hi);

                Range left_range = { current_range.lo, p - 1 };
                Range right_range = { p + 1, current_range.hi };

                {
                    std::lock_guard<std::mutex> lock(queue_mutex);

                    if (left_range.lo < left_range.hi) {
                        task_queue.push(left_range);
                        active_tasks++;
                    }
                    if (right_range.lo < right_range.hi) {
                        task_queue.push(right_range);
                        active_tasks++;
                    }
                }

                cv.notify_all();
            }

            // Mark this task as completed
            active_tasks--;

            // If no active tasks remain and queue is empty, signal shutdown to all threads
            if (active_tasks.load() == 0) {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (task_queue.empty() && active_tasks.load() == 0) {
                    shutdown.store(true);
                    cv.notify_all();
                }
            }
        }
        };

    // Spawn a fixed number of worker threads equal to the number of cores
    std::vector<std::thread> workers;
    workers.reserve(num_cores);
    for (unsigned int i = 0; i < num_cores; ++i) {
        workers.emplace_back(worker_func, i);
    }

    // Join all worker threads when work is complete
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}


int compareDoubles(const void* a, const void* b) {
    double arg1 = *static_cast<const double*>(a);
    double arg2 = *static_cast<const double*>(b);

    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

void checkIfSorted(const std::vector<double>& vec) {
	bool result = true;
    for (size_t i = 1; i < vec.size(); ++i) {
        if (vec[i - 1] > vec[i]) {
            result = false;
            break;
        }
    }
    if (result) {
        std::cout << "Checked: array is sorted." << std::endl;
    }
    else {
        std::cout << "Checked: array is NOT sorted." << std::endl;
    }
}

double findMax(const std::vector<double>& vec) {
    double max_val = vec[0];
    for (const auto& val : vec) {
        if (val > max_val) {
            max_val = val;
        }
    }
    return max_val;
}

double findMin(const std::vector<double>& vec) {
    double min_val = vec[0];
    for (const auto& val : vec) {
        if (val < min_val) {
            min_val = val;
        }
    }
    return min_val;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <target_count>\n";
        return EXIT_FAILURE;
    }

    size_t target_count = 0;
    try {
        size_t idx = 0;
        long long parsed_val = std::stoll(argv[1], &idx);
        if (parsed_val <= 0 || idx != std::string(argv[1]).length()) {
            throw std::out_of_range("Invalid positive integer.");
        }
        target_count = static_cast<size_t>(parsed_val);
    }
    catch (const std::exception&) {
        std::cerr << "Error: Please provide a valid positive integer for target_count.\n";
        return EXIT_FAILURE;
    }

    constexpr double min_val = 1.0;         // Minimum range value
    constexpr double max_val = 100.0;       // Maximum range value

    std::cout << "Generation of array of double values was started..." << std::endl;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<double> dis(min_val, max_val);
    std::unordered_set<double> unique_numbers;
    while (unique_numbers.size() < target_count) {
        double random_value = dis(gen);
        unique_numbers.insert(random_value);
    }
    std::vector<double> vec(unique_numbers.begin(), unique_numbers.end());
    std::cout << "Generated array of " << unique_numbers.size() << " double values" << std::endl;
    std::cout << "Max is " << findMax(vec) << " min is " << findMin(vec) << std::endl;

    checkIfSorted(vec);

    auto start = std::chrono::steady_clock::now();
    // -------------------------------------------------------------

    if(MY_OWN_SORT){
        std::cout << "Using a custom sort function..." << std::endl;
        int32_t last_index = static_cast<int32_t>(vec.size() - 1);

        // uncomment one of 3 options
        //MyQsortRecursive(vec, 0, last_index);
        //MyQsortIterative(vec, 0, last_index);
        MyQsortParallelIterative(vec, 0, last_index);
    }
    else {
        //std::cout << "Using std::qsort..." << std::endl;
        //std::qsort(vec.data(), vec.size(), sizeof(double), compareDoubles);
        std::cout << "Using std::sort..." << std::endl;
        std::sort(vec.begin(), vec.end());
	}

    // -------------------------------------------------------------
    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double, std::milli> elapsed_ms = end - start;
    std::cout << "Execution time: " << elapsed_ms.count() << " ms" << std::endl << std::endl;
    checkIfSorted(vec);
    return EXIT_SUCCESS;
}