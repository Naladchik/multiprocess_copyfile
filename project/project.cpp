#include <iostream>
#include <chrono>
#include <thread>
#include <boost/program_options.hpp>
#include <filesystem>
#include <fstream>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>

using namespace std;
using namespace std::literals;

namespace po = boost::program_options;
namespace ip = boost::interprocess;

constexpr uint16_t kChunksCount{ 2 };
constexpr uint16_t kChunkSize{ 0xffff };

constexpr int kNotFound{ -1 };
constexpr streamsize kReadyToRead{ 0 };

constexpr uint16_t kStringMaxSize{ 256 };

enum class Role : unsigned int {
    CREATOR = 0,
    USER = 1,
    READER = 2,
    WRITER = 3
};


struct DataChunk {
    array<char, kChunkSize> data{ };
    streamsize size{ 0 };
    streamsize index{ kReadyToRead };
};

class SharedVars {
public:
    ip::interprocess_mutex mtx;
    ip::interprocess_condition cv;

    bool finish = false;
    bool ongoing = false;

    array<char, kStringMaxSize> source{ 0 };
    array<char, kStringMaxSize> destination{ 0 };


    void set_source(const string& src) {
        for (uint16_t i = 0; src[i] != '\0'; i++) {
			source[i] = src[i];
        }
    }

    void set_destination(const string& dest) {
        for (uint16_t i = 0; dest[i] != '\0'; i++) {
            destination[i] = dest[i];
        }
    }

    bool compare_source(const string& src) {
        // namespace std has no member string_view (some known problem)
        return strncmp(source.data(), src.c_str(), kStringMaxSize) == 0;
    }

    bool compare_destination(const string& dest) {
        // namespace std has no member string_view (some known problem)
        return strncmp(destination.data(), dest.c_str(), kStringMaxSize) == 0;
    }

    array<DataChunk, kChunksCount> chunks;

    int get_next_index(int index) {
        for (size_t i = 0; i < chunks.size(); ++i) {
            if (chunks[i].index == index) {
                return static_cast<int>(i);
            }
        }
        return kNotFound;
    }
};

struct SharedMemoryLayout {
    atomic<bool> ready{true};
    SharedVars vars;
};

int main(int argc, char* argv[])
{
    // Declare the supported options.
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "produce help message")
        ("source", po::value<string>(), "set file to copy from")
        ("destination", po::value<string>(), "set file to copy to")
        ("memory", po::value<string>(), "name of shared memory object")
        ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) { // [m] vm. contains is supposed to be here
        std::cout << desc << endl;
        return 1;
    }

    if (!vm.count("source")) {
        std::cout << "Source file was not set." << endl;
    }
    else if (!vm.count("destination")) {
        std::cout << "Destination file was not set." << endl;
    }
    else if (!vm.count("memory")) {
        std::cout << "Memory name was not set." << endl;
    }
    else {
        /*cout << "Source: " << vm["source"].as<string>() << endl;
        cout << "Destination: " << vm["destination"].as<string>() << endl;
        cout << "Memory: " << vm["memory"].as<string>() << endl;*/

        // ============== MAIN LOGIC IS HERE ======================================

        unique_ptr<ip::shared_memory_object> shm_obj_ptr;
        int mem_role, file_role;
        SharedVars* sch_vars;
        SharedMemoryLayout* shm;
        const auto& mem_name = vm["memory"].as<string>();
        bool try_to_join = true;
        uint16_t attemts = 20;

        // -------------- loop for testing memory and current situation -----------
    once_again:
        // memory is created or opened if already exists
        try {
            shm_obj_ptr = make_unique<ip::shared_memory_object>(ip::create_only, mem_name.c_str(), ip::read_write);
            shm_obj_ptr->truncate(sizeof(SharedMemoryLayout));
            mem_role = static_cast<unsigned int>(Role::CREATOR);
        }
        catch (const ip::interprocess_exception&) {
            this_thread::sleep_for(chrono::microseconds(10));  //workaround for unknown yet issue
            shm_obj_ptr = make_unique<ip::shared_memory_object>(ip::open_only, mem_name.c_str(), ip::read_write);
            mem_role = static_cast<unsigned int>(Role::USER);
        }

        ip::mapped_region region(*shm_obj_ptr, ip::read_write);
        //ip::shared_memory_object::remove(vm["memory"].as<string>().c_str());


        if (mem_role == static_cast<unsigned int>(Role::CREATOR)) {
            shm = new (region.get_address()) SharedMemoryLayout;
            sch_vars = &shm->vars;
            {
                ip::scoped_lock<boost::interprocess::interprocess_mutex> lock(sch_vars->mtx);
                sch_vars->set_source(vm["source"].as<string>());
                sch_vars->set_destination(vm["destination"].as<string>());
            }
            file_role = static_cast<unsigned int>(Role::READER);
            try_to_join = false;
            std::cout << "CREATOR: STARTED" << endl;
        }
        else {
            shm = static_cast<SharedMemoryLayout*>(region.get_address());
            while (!shm->ready.load(memory_order_acquire)) {}
            sch_vars = &shm->vars;
            {
                ip::scoped_lock<boost::interprocess::interprocess_mutex> lock(sch_vars->mtx);
                if (sch_vars->ongoing) {
                    if ((sch_vars->compare_source(vm["source"].as<string>()) &&
                        sch_vars->compare_destination(vm["destination"].as<string>()))) {
                        std::cout << "NEW: FILES DUPLICATION. EXIT." << endl;
                        return 0;
                    }
                    else {
                        if (attemts == 0) return 0;
                        std::cout << "NEW: WAITING. ONE MORE TRY." << endl;
                        attemts--;
                    }
                }
                else {
                    if ((sch_vars->compare_source(vm["source"].as<string>()) &&
                        sch_vars->compare_destination(vm["destination"].as<string>()))) {
                        sch_vars->ongoing = true;
                        try_to_join = false;
                        std::cout << "USER: STARTED" << endl;
                    }
                    else {
                        if (attemts == 0) return 0;
                        std::cout << "NEW: NO PAIR. ONE MORE TRY." << endl;
                        attemts--;
                    }
                }
            }
            file_role = static_cast<unsigned int>(Role::WRITER);
        }
        if (try_to_join) {
            this_thread::sleep_for(chrono::milliseconds(500));
            goto once_again;
        }


        // -------------- fork READER-WRITER with two loops inside ----------------
        if (file_role == static_cast<unsigned int>(Role::READER)) {  // READER
            ifstream input_file(vm["source"].as<string>(), ios::binary);

            size_t chunkIndex{ kReadyToRead };
            size_t readyToReadIndex{ 0 };

            while (true) {
                auto& chunk = sch_vars->chunks[readyToReadIndex];
                input_file.read(chunk.data.data(), kChunkSize);
                chunk.size = input_file.gcount();
                {
                    ip::scoped_lock<boost::interprocess::interprocess_mutex> lock(sch_vars->mtx);
                    chunk.index = ++chunkIndex;
                    readyToReadIndex = sch_vars->get_next_index(kReadyToRead);
                    sch_vars->cv.notify_one();
                    if (chunk.size != kChunkSize) {
                        break;
                    }
                    if (kNotFound == readyToReadIndex) {
                        if (!sch_vars->cv.wait_for(lock, chrono::seconds(10), [&readyToReadIndex, sch_vars] {
                            readyToReadIndex = sch_vars->get_next_index(kReadyToRead);
                            return kNotFound != readyToReadIndex;
                            })) {
                            std::cout << "Timeout: No notification within 10 seconds." << endl;
                            goto stop;
                        }
                    }
                }
            }
        }
        else {  // WRITER
            ofstream output_file(vm["destination"].as<string>(), ios::binary);

            int chunkIndex{ kReadyToRead + 1 };
            int readyToWriteIndex{ kNotFound };
            while (true) {
                {
                    ip::scoped_lock<boost::interprocess::interprocess_mutex> lock(sch_vars->mtx);
                    if (readyToWriteIndex != kNotFound) {
                        auto& chunk = sch_vars->chunks[readyToWriteIndex];
                        chunk.index = kReadyToRead;
                        sch_vars->cv.notify_one();
                        readyToWriteIndex = sch_vars->get_next_index(chunkIndex);
                    }
                    if (readyToWriteIndex == kNotFound) {
                        sch_vars->cv.wait(lock, [&readyToWriteIndex, &chunkIndex, sch_vars] {
                            readyToWriteIndex = sch_vars->get_next_index(chunkIndex);
                            return kNotFound != readyToWriteIndex;
                            });
                    }
                    ++chunkIndex;
                }
                auto& chunk = sch_vars->chunks[readyToWriteIndex];
                output_file.write(chunk.data.data(), chunk.size);
                if (chunk.size != kChunkSize) {
                    break;
                }
            }
        }

        if (mem_role == static_cast<unsigned int>(Role::CREATOR)) {
            {  // wait until user ends
                ip::scoped_lock<boost::interprocess::interprocess_mutex> lock(sch_vars->mtx);
                sch_vars->cv.wait(lock, [sch_vars] { return sch_vars->finish; });
            }
        stop:
            ip::shared_memory_object::remove(vm["memory"].as<string>().c_str());
            std::cout << "CREATOR: FINISHED" << endl;
        }
        else {
            {  //user ends and sends signal to creator
                ip::scoped_lock<boost::interprocess::interprocess_mutex> lock(sch_vars->mtx);
                sch_vars->finish = true;
                sch_vars->cv.notify_one();
            }

            std::cout << "USER: FINISHED" << endl;
        }
    }

    return 0;
}