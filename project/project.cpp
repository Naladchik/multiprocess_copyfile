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
    atomic<bool> initialized = false;
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
        ("source", po::value<string>(), "set file to copy from")
        ("destination", po::value<string>(), "set file to copy to")
        ("memory", po::value<string>(), "name of shared memory object")
        ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) { // [m] vm. contains is supposed to be here
        cout << desc << endl;
        return 1;
    }

    if (!vm.count("source")) {
        cout << "Source file was not set." << endl;
    }
    else if (!vm.count("destination")) {
        cout << "Destination file was not set." << endl;
    }
    else if (!vm.count("memory")) {
        cout << "Memory name was not set." << endl;
    }
    else {
        /*cout << "Source: " << vm["source"].as<string>() << endl;
        cout << "Destination: " << vm["destination"].as<string>() << endl;
        cout << "Memory: " << vm["memory"].as<string>() << endl;*/

        // ============== MAIN LOGIC IS HERE ======================================
        unique_ptr<ip::shared_memory_object> shm_obj_ptr;

        int role;

        const auto& mem_name = vm["memory"].as<string>();

        // memory is created or opened if already exists
        try {
            shm_obj_ptr = make_unique<ip::shared_memory_object>(ip::create_only, mem_name.c_str(), ip::read_write);
            shm_obj_ptr->truncate(sizeof(SharedMemoryLayout)); // [f] sizeof(SharedVars)
            role = static_cast<unsigned int>(Role::CREATOR);
            cout << "CREATOR: STARTED" << endl;
        }
        catch (const ip::interprocess_exception&) {
            this_thread::sleep_for(chrono::microseconds(10));  //workaround for unknown yet issue
            shm_obj_ptr = make_unique<ip::shared_memory_object>(ip::open_only, mem_name.c_str(), ip::read_write);
            role = static_cast<unsigned int>(Role::USER);
            cout << "USER: STARTED" << endl;
        }

        ip::mapped_region region(*shm_obj_ptr, ip::read_write);

        if (role == static_cast<unsigned int>(Role::CREATOR)) {
            SharedMemoryLayout* shm = static_cast<SharedMemoryLayout*>(region.get_address());
            new (&shm->vars) SharedVars;
            shm->ready.store(true, memory_order_release);

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
                        if (!sch_vars->cv.wait_for(lock, chrono::seconds(10), [&readyToReadIndex, sch_vars] {
                            readyToReadIndex = sch_vars->get_next_index(kReadyToRead);
                            return kNotFound != readyToReadIndex;
                            })) {
                            cout << "Timeout: No notification within 10 seconds.\n";
                            goto stop;
                        }
                    }
                }
            }

            {  // wait until user ends
                ip::scoped_lock<boost::interprocess::interprocess_mutex> lock(sch_vars->mtx);
                sch_vars->cv.wait(lock, [sch_vars] { return sch_vars->finish; });
            }

        stop:
            ip::shared_memory_object::remove(vm["memory"].as<string>().c_str());
            cout << "CREATOR: FINISHED" << endl;
        }
        else { // role == USER
            SharedMemoryLayout* shm = static_cast<SharedMemoryLayout*>(region.get_address());
            while (!shm->ready.load(memory_order_acquire)) {}
            SharedVars* sch_vars = &shm->vars;

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


            {  //user ends and sends signal to creator
                ip::scoped_lock<boost::interprocess::interprocess_mutex> lock(sch_vars->mtx);
                sch_vars->finish = true;
                sch_vars->cv.notify_one();
            }

            cout << "USER: FINISHED" << endl;
        }
    }

    return 0;
}
