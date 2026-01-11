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

constexpr unsigned int chunk_size = 65535;
constexpr unsigned int mem_size = 3 * chunk_size;

constexpr unsigned int CREATOR = 0;
constexpr unsigned int USER = 1;

class SharedVars {
public:
    ip::interprocess_mutex mtx;
    ip::interprocess_condition cv;

    char buf[2][chunk_size] = {1};

    bool was_written = false;
    bool user_is_on = false;
    int actual_size = 0;
    bool creator_finish = false;
	bool user_finish = false;
};

unsigned char buf_prod = 0;  // number of buffer for the next operation of producer
unsigned char buf_cons = 0;  // number of buffer for the next operation of consumer

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
            shm_obj_ptr->truncate(sizeof(int) * mem_size);
            role = CREATOR;
            std::cout << "CREATOR: STARTED" << std::endl;
        }
        catch (const ip::interprocess_exception&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            shm_obj_ptr = std::make_unique<ip::shared_memory_object>(ip::open_only, mem_name.c_str(), ip::read_write);
            role = USER;
            std::cout << "USER: STARTED" << std::endl;
        }

        ip::mapped_region region(*shm_obj_ptr, ip::read_write);
        //if (role == USER) { std::cout << "LABEL: 1" << std::endl; }
        SharedVars* sch_vars = new (region.get_address()) SharedVars;
        
        //if (role == USER) { std::cout << "LABEL: 2" << std::endl; }

        if(role == CREATOR){
            while(!sch_vars->user_is_on){
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            std::ifstream input_file(vm["source"].as<std::string>(), std::ios::binary);
            
            while (!input_file.eof()) {

                input_file.read(sch_vars->buf[buf_prod], chunk_size);
                buf_prod ^= 0x01;

                {
                    ip::scoped_lock<ip::interprocess_mutex> lock(sch_vars->mtx);
                    sch_vars->actual_size = (unsigned int) input_file.gcount();  // transmit size to consumer
                    sch_vars->was_written = true;
                    sch_vars->cv.notify_one();
                    //sch_vars->cv.wait(lock, [sch_vars] { return sch_vars->was_written; });
                }
            }

            sch_vars->creator_finish = true;

			bool sig_ok = false;
            for (int i = 0; i < 100; i++){
                if(sch_vars->user_finish){
                    sig_ok = true;
                    break;
				}
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if(!sig_ok){
				std::cout << "CREATOR: TIMEOUT WAITING FOR USER" << std::endl;
			}

            ip::shared_memory_object::remove(vm["memory"].as<std::string>().c_str());
            std::cout << "CREATOR: FINISHED" << std::endl;

		}
		else { // role == USER
            std::ofstream output_file(vm["destination"].as<std::string>(), std::ios::binary);

            sch_vars->user_is_on = true;

            while (true) {
                {
                    ip::scoped_lock<boost::interprocess::interprocess_mutex> lock(sch_vars->mtx);
                    sch_vars->cv.wait(lock, [sch_vars] { return sch_vars->was_written == true;; });
                    sch_vars->was_written = false;
                    sch_vars->cv.notify_one();
                }

                output_file.write(sch_vars->buf[buf_cons], sch_vars->actual_size);
                buf_cons ^= 0x01;
                
                if (sch_vars->creator_finish) break;
            }

            sch_vars->user_finish = true;

            std::cout << "USER: FINISHED" << std::endl;
        }
    }

    return 0;
}