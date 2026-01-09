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

namespace po = boost::program_options;
namespace ip = boost::interprocess;

ip::interprocess_mutex mtx;
ip::interprocess_condition cv;

constexpr unsigned int max_memory_size = 1048576; // 1 MB
constexpr unsigned int chunk_size = 65535;
bool memory_not_needed = false;

constexpr unsigned int CREATOR = 0;
constexpr unsigned int USER = 1;
constexpr unsigned int MEM_SIZE = 2 * 65535;

unsigned char buf_prod = 0;  // number of buffer for the next operation of producer
unsigned char buf_cons = 0;  // number of buffer for the next operation of consumer
unsigned char prod_num = 2;  // number of available for procurer buffers
unsigned char cons_num = 0;  // number of available for consumer buffers
int actual_size;
bool creator_finish = false;
bool user_finish = false;

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

    if (vm.count("help")) {
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
	}else{
        /*std::cout << "Source: " << vm["source"].as<std::string>() << std::endl;
		std::cout << "Destination: " << vm["destination"].as<std::string>() << std::endl;
		std::cout << "Memory: " << vm["memory"].as<std::string>() << std::endl;*/

        // ============== MAIN LOGIC IS HERE ======================================
        std::unique_ptr<ip::shared_memory_object> shm_obj_ptr;

        int role;

	    const auto & mem_name = vm["memory"].as<std::string>();

        // memory is created or opened if already exists
        try {
            shm_obj_ptr = std::make_unique<ip::shared_memory_object>(ip::create_only, mem_name.c_str(), ip::read_write);
            shm_obj_ptr->truncate(sizeof(int) * MEM_SIZE);
            role = CREATOR;
        }
        catch (const ip::interprocess_exception& ex) {
            shm_obj_ptr = std::make_unique<ip::shared_memory_object>(ip::open_only, mem_name.c_str(), ip::read_write);
            role = USER;
        }

        ip::mapped_region region(*shm_obj_ptr, ip::read_write);
        char* shm_array = static_cast<char*>(region.get_address());
        char (*buf)[2] = reinterpret_cast<char (*)[2]>(shm_array);

        if(role == CREATOR){

            std::ifstream input_file(vm["source"].as<std::string>(), std::ios::binary);
            
            while (!input_file.eof()) {
                {
                    ip::scoped_lock<boost::interprocess::interprocess_mutex> lock(mtx);
                    while (prod_num <= 0) {
                        cv.wait(lock);
                    }
                    prod_num--;
                }

                input_file.read(buf[buf_prod], chunk_size);
                buf_prod ^= 0x01;

                {
                    ip::scoped_lock<boost::interprocess::interprocess_mutex> lock(mtx);
                    actual_size = input_file.gcount();  // transmit size to consumer
                    cons_num++;
                    cv.notify_one();
                }
            }
            creator_finish = true;

            while(!user_finish){
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}

            ip::shared_memory_object::remove(vm["memory"].as<std::string>().c_str());
        }
        else {
            std::ofstream output_file(vm["destination"].as<std::string>(), std::ios::binary);
            
            while (true) {
                {
                    ip::scoped_lock<boost::interprocess::interprocess_mutex> lock(mtx);
                    cv.wait(lock, [] { return cons_num > 0; });
                    cons_num--;
                }
                output_file.write(buf[buf_cons], actual_size);
                buf_cons ^= 0x01;
                {
                    ip::scoped_lock<boost::interprocess::interprocess_mutex> lock(mtx);
                    prod_num++;
                    if (creator_finish && (cons_num == 0)) break;
                    cv.notify_one();
                }
            }
			user_finish = true;
        }
    }

    return 0;
}