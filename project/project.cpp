#include <iostream>
#include <chrono>
#include <thread>
#include <boost/program_options.hpp>
#include <filesystem>
#include <fstream>
#include <boost/interprocess/shared_memory_object.hpp>

namespace po = boost::program_options;
namespace ip = boost::interprocess;

constexpr unsigned int max_memory_size = 1048576; // 1 MB
constexpr unsigned int chunk_size = 65535;
bool memory_not_needed = false;

bool memory_exists(const std::string& name) {
    try {
        ip::shared_memory_object shm(
            ip::open_only,
            name.c_str(),
            ip::read_write
        );
        return true; // Memory object exists
    }
    catch (const ip::interprocess_exception& ex) {
        return false; // Memory object does not exist
    }
}

class MemoryHolder{
public:
    ip::shared_memory_object shm;
	std::string memory_name;
	bool remove_needed = true;

    MemoryHolder(const std::string& name, std::size_t size, bool create)
    {	
		memory_name.append(name);
        remove_needed = create;
        if(create){
            shm = ip::shared_memory_object(
                ip::create_only,
                name.c_str(),
                ip::read_write
            );
            shm.truncate(size);
            std::cout << "Memory object created. " << std::endl;
        }else{
            shm = ip::shared_memory_object(
                ip::open_only,
                name.c_str(),
                ip::read_write
            );
            std::cout << "Memory object opened. " << std::endl;
        }
    }

    ~MemoryHolder()
    {
        if(remove_needed){
            ip::shared_memory_object::remove(memory_name.c_str());
            std::cout << "Memory object removed. " << std::endl;
        }
        else {
			std::cout << "Memory object released. " << std::endl;
        }
    }
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

        constexpr unsigned int CREATOR = 0;
        constexpr unsigned int USER = 1;
		int role = CREATOR; // 0 - creator, 1 - user

        if( memory_exists(vm["memory"].as<std::string>())){
            role = USER;
		}

        if(role == USER){

            std::ifstream input_file(vm["source"].as<std::string>(), std::ios::binary);
            if (!input_file) {
                std::cerr << "Error opening source file: " << vm["source"].as<std::string>() << std::endl;
                return 1;
            }else{ // here the work with file
                MemoryHolder memory_holder(
                    vm["memory"].as<std::string>(),
                    max_memory_size,
                    false
				);



				memory_not_needed = true;
            }
		}else {  // role == CREATOR

            std::ofstream input_file(vm["destination"].as<std::string>(), std::ios::binary);
            if (!input_file) {
                std::cerr << "Error opening source file: " << vm["destination"].as<std::string>() << std::endl;
                return 1;
            }else { // here the work with file
                MemoryHolder memory_holder(
                    vm["memory"].as<std::string>(),
                    max_memory_size,
                    true
                );




				do {} while (!memory_not_needed);
            }
        }
    }

    return 0;
}