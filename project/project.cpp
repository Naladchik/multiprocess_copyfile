#include <iostream>
#include <chrono>
#include <thread>
#include <random>
#include <unordered_set>
#include <vector>

constexpr bool MY_OWN_SORT = true; // Set to true to use custom sort, false to use std::sort

void MyQsort(std::vector<double>& vec) {
	vec[1] = 0.0; // Set the second element to 0.0
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

    auto start = std::chrono::steady_clock::now();
    // -------------------------------------------------------------

    constexpr double min_val = 1.0;         // Minimum range value
    constexpr double max_val = 100.0;       // Maximum range value

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

    if(MY_OWN_SORT){
        std::cout << "Using MyQsort..." << std::endl;
        MyQsort(vec);
    }
    else {
        std::cout << "Using std::qsort..." << std::endl;
        std::qsort(vec.data(), vec.size(), sizeof(double), compareDoubles);
        //std::cout << "Using std::sort..." << std::endl;
        //std::sort(vec.begin(), vec.end());
	}

    checkIfSorted(vec);

    // -------------------------------------------------------------
    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double, std::milli> elapsed_ms = end - start;
    std::cout << "Execution time: " << elapsed_ms.count() << " ms" << std::endl << std::endl;
    return EXIT_SUCCESS;
}