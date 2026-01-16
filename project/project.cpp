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

enum class Role : unsigned int {
    CREATOR = 0,
    USER = 1
};


struct DataChunk {
    array<char, kChunkSize> data{ };
    streamsize size{ 0 };
    streamsize index{ kReadyToRead };
};

class SharedVars {
public:
    std::atomic<bool> initialized = false;
    ip::interprocess_mutex mtx;
    ip::interprocess_condition cv;

    array<DataChunk, kChunksCount> chunks;

    int get_next_index(int index) {
        for (size_t i = 0; i < chunks.size(); ++i) {
            if (chunks[i].index == index) {
                return static_cast<int>(i);
            }
        }
        return kNotFound;
    }

    bool finish = false;
};

struct SharedMemoryLayout {
    atomic<bool> ready;
    SharedVars vars;
};

int main(int argc, char* argv[])
{
    // Declare the supported options.
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "produce help message")
        ("source", po::value<std::string>(), "set file to copy from")
        ("destination", po::value<std::string>(), "set file to copy to")
        ("memory", po::value<std::string>(), "name of shared memory object")
        ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) { // [m] vm. contains is supposed to be here
        std::cout << desc << std::endl;
        return 1;
    }

    if (!vm.count("source")) {
        std::cout << "Source file was not set." << std::endl;
    }
    else if (!vm.count("destination")) {
        std::cout << "Destination file was not set." << std::endl;
    }
    else if (!vm.count("memory")) {
        std::cout << "Memory name was not set." << std::endl;
    }
    else {
        /*std::cout << "Source: " << vm["source"].as<std::string>() << std::endl;
        std::cout << "Destination: " << vm["destination"].as<std::string>() << std::endl;
        std::cout << "Memory: " << vm["memory"].as<std::string>() << std::endl;*/

        // ============== MAIN LOGIC IS HERE ======================================
        std::unique_ptr<ip::shared_memory_object> shm_obj_ptr;

        int role;

        const auto& mem_name = vm["memory"].as<std::string>();

        // memory is created or opened if already exists
        try {
            shm_obj_ptr = std::make_unique<ip::shared_memory_object>(ip::create_only, mem_name.c_str(), ip::read_write);
			shm_obj_ptr->truncate(sizeof(SharedMemoryLayout)); // [f] sizeof(SharedVars)
            role = static_cast<unsigned int>(Role::CREATOR);
            std::cout << "CREATOR: STARTED" << std::endl;
        }
        catch (const ip::interprocess_exception&) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));  //workaround for unknown yet issue
            shm_obj_ptr = std::make_unique<ip::shared_memory_object>(ip::open_only, mem_name.c_str(), ip::read_write);
            role = static_cast<unsigned int>(Role::USER);
            std::cout << "USER: STARTED" << std::endl;
        }

        ip::mapped_region region(*shm_obj_ptr, ip::read_write);
        //ip::shared_memory_object::remove(vm["memory"].as<string>().c_str());

        if (role == static_cast<unsigned int>(Role::CREATOR)) {
            SharedMemoryLayout* shm = static_cast<SharedMemoryLayout*>(region.get_address());
            new (&shm->vars) SharedVars;
            shm->ready.store(true, std::memory_order_release);

            SharedVars* sch_vars = &shm->vars;

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
                        sch_vars->cv.wait(lock, [&readyToReadIndex, sch_vars] {
                            readyToReadIndex = sch_vars->get_next_index(kReadyToRead);
                            return kNotFound != readyToReadIndex;
                            });
                    }
                }
            }

            {  // wait until user ends
                ip::scoped_lock<boost::interprocess::interprocess_mutex> lock(sch_vars->mtx);
                sch_vars->cv.wait(lock, [sch_vars] { return sch_vars->finish; });
            }

            ip::shared_memory_object::remove(vm["memory"].as<string>().c_str());
            std::cout << "CREATOR: FINISHED" << std::endl;
        }
        else { // role == USER
            SharedMemoryLayout* shm = static_cast<SharedMemoryLayout*>(region.get_address());
            while (!shm->ready.load(std::memory_order_acquire)) {}
            SharedVars* sch_vars = &shm->vars;

            std::ofstream output_file(vm["destination"].as<std::string>(), std::ios::binary);

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


            {  //user ends and sends signal to creator
                ip::scoped_lock<boost::interprocess::interprocess_mutex> lock(sch_vars->mtx);
                sch_vars->finish = true;
                sch_vars->cv.notify_one();
            }

            std::cout << "USER: FINISHED" << std::endl;
        }
    }

    return 0;
}
