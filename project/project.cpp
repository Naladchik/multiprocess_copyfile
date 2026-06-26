/*
┌──────────────────┬───────────────────────────────────────────────────┐
│ Berkeley sockets │               Boost.Asio equivalent               │
├──────────────────┼───────────────────────────────────────────────────┤
│ socket()         │ constructing tcp::socket or tcp::acceptor         │
├──────────────────┼───────────────────────────────────────────────────┤
│ bind()           │ passing tcp::endpoint to the acceptor constructor │
├──────────────────┼───────────────────────────────────────────────────┤
│ listen()         │ also handled inside the acceptor constructor      │
├──────────────────┼───────────────────────────────────────────────────┤
│ accept()         │ acceptor.accept(socket)                           │
├──────────────────┼───────────────────────────────────────────────────┤
│ connect()        │ boost::asio::connect(socket, endpoints)           │
├──────────────────┼───────────────────────────────────────────────────┤
│ send() / write() │ boost::asio::write(socket, buffer)                │
├──────────────────┼───────────────────────────────────────────────────┤
│ recv() / read()  │ boost::asio::read_until(socket, buffer, '\n')     │
├──────────────────┼───────────────────────────────────────────────────┤
│ close()          │ socket.close()                                    │
└──────────────────┴───────────────────────────────────────────────────┘

The main differences are :

-bind + listen are merged into the tcp::acceptor constructor — Asio assumes you always want both when you create an acceptor, so there's no reason to
separate them
- Resolver is added — Berkeley sockets have getaddrinfo() as a separate C function; Asio wraps it into tcp::resolver to fit the same object model
- Buffers are typed — instead of raw void* +length you pass boost::asio::buffer(data) which carries the size with it, preventing a whole class of bugs
- Async is a first - class option — every operation has an async variant(async_accept, async_read_until, etc.) that Berkeley sockets don't have natively
*/

#include <boost/asio.hpp>
#include <boost/program_options.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <array>
#include <cstdint>
#include <cstring>

using namespace std;
using boost::asio::ip::tcp;   // shorthand so we can write tcp::socket etc.
namespace po = boost::program_options;
namespace fs = std::filesystem;

const int PORT = 12345;

constexpr uint16_t kChunkSize{ 0xffff };

constexpr uint16_t max_string{ 64 };

array<char, kChunkSize> buffer{};

#pragma pack(push, 1)
struct tFileInfo {
    uint64_t size;
    char name[max_string];
};
#pragma pack(pop)

string file_to_send = "";

// ---------------------------------------------------------------------------
// SERVER
// ---------------------------------------------------------------------------
static void run_server()
{
    //tFileInfo file_info{};
    uint64_t size_of_read = sizeof(tFileInfo);
    streamsize bytes_read = sizeof(tFileInfo);

    boost::asio::io_context io;
    
    tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), PORT));

    std::cout << "[Server] Status: LISTENING on port " << PORT << endl;
    std::cout << "[Server] Waiting for a client to connect..." << endl;

    tcp::socket socket(io);
    acceptor.accept(socket);   // <-- blocks here

    std::cout << "[Server] Status: CONNECTED  <-- "
         << socket.remote_endpoint().address().to_string()
         << ":" << socket.remote_endpoint().port() << endl;

    boost::system::error_code error;

    bytes_read = boost::asio::read(socket, boost::asio::buffer(buffer, size_of_read), error);

    if (error)
    {
        std::cout << "[Server] Error during receiving file info. Status: ERROR – " << error.message() << endl;
        return;
    }

	void* p = buffer.data();
	tFileInfo* s_p = static_cast<tFileInfo*>(p);

    size_of_read = kChunkSize;
    uintmax_t file_size = s_p->size;
	string file_name = string(s_p->name);  // still needed for the deletion in case of incomplete transfer

    std::cout << "[Server] Received file info: " << s_p->name << " (" << s_p->size << " bytes)" << endl;

	string output_filename = "received/" + string(s_p->name);

    ofstream output_file(output_filename, ios::out | ios::binary | ios::noreplace);

    if (!output_file.is_open()) {
        std::cerr << "[Server] File already exists! Overwrite prevented." << std::endl;
        return; // Stop here, do not continue writing
    }

	bool file_received = false;
    uint64_t total_bytes_received = 0;  // only file bytes, file info is not counted

    while (true)
    {
        bytes_read = boost::asio::read(socket, boost::asio::buffer(buffer, size_of_read), error);

        if (bytes_read > 0) {  
            output_file.write(buffer.data(), bytes_read);
            total_bytes_received += bytes_read;
            if (total_bytes_received == file_size) {
                file_received = true;
                size_of_read = 0;
                std::cout << "[Server] Received the whole file " << endl;
                break;
            }

            if (total_bytes_received > file_size) {
                std::cout << "[Server] Received more data than expected! " << total_bytes_received << endl;
                break;
            }

            if (file_size - total_bytes_received < kChunkSize) size_of_read  = file_size - total_bytes_received;
        }            

        if (error == boost::asio::error::eof)
        {
            std::cout << "[Server] Status: DISCONNECTED (client closed the connection)" << endl;
            if(!file_received){
                std::cout << "[Server] Status: ERROR – File transfer incomplete. Received " << total_bytes_received << " bytes out of " << file_size << " bytes." << endl;

                output_file.close();

                fs::path file_path = "received/" + file_name;

                try {
                    if (fs::remove(file_path)) {
                        std::cout << "[Server] File successfully deleted.\n";
                    }
                    else {
                        std::cout << "[Server] File did not exist.\n";
                    }
                }
                catch (const fs::filesystem_error& err) {
                    std::cerr << "[Server] Filesystem error: " << err.what() << '\n';
                }
			}
            break;
        }

        if (error)
        {
            std::cout << "[Server] Status: ERROR – " << error.message() << endl;
            break;
        }
    }
}


// ---------------------------------------------------------------------------
// CLIENT
// ---------------------------------------------------------------------------
static void run_client()
{
    boost::asio::io_context io;

    uintmax_t file_size;

    ifstream input_file(file_to_send, ios::binary);

    if (!input_file) {
        std::cout << "[Client] Failed to open source file " << file_to_send  << endl;
        return;
    }
    else {
        file_size = filesystem::file_size(file_to_send);
        std::cout << "[Client] File " << file_to_send << " with size " << file_size << " will be sent." << endl;
    }    

	//tFileInfo structure is placed directly to buffer
	void* p = buffer.data();
    tFileInfo* s_p = static_cast<tFileInfo*>(p);
    strncpy_s(s_p->name, sizeof(s_p->name), file_to_send.c_str(), _TRUNCATE);
    s_p->name[sizeof(s_p->name) - 1] = '\0'; // Ensure null-termination
    s_p->size = static_cast<uint64_t>(file_size);

    tcp::resolver resolver(io);

    auto endpoints = resolver.resolve("127.0.0.1", to_string(PORT));

    tcp::socket socket(io);

    std::cout << "[Client] Status: CONNECTING to 127.0.0.1:" << PORT << " ..." << endl;

    boost::system::error_code error;

    boost::asio::connect(socket, endpoints, error);   // <-- blocks here

    if (error)
    {
        std::cout << "[Client] Status: FAILED to connect – " << error.message() << endl;
        std::cout << "[Client] Make sure the server is running first." << endl;
        return;
    }

    std::cout << "[Client] Status: CONNECTED to server" << endl;
    std::cout << "[Client] Type a message and press Enter to send." << endl;
    std::cout << "[Client] Type 'quit' to disconnect." << endl;

	bool file_info_sent = false;
    streamsize bytes_read = sizeof(tFileInfo);
    uint64_t total_bytes_transmitted = 0;  // only file bytes, file info is not counted

    while (true)
    {
        if(file_info_sent){
            input_file.read(buffer.data(), kChunkSize);
            bytes_read = input_file.gcount();
			total_bytes_transmitted += bytes_read;
        }
        else {
			//memcpy(buffer.data(), &file_info, sizeof(file_info));
            file_info_sent = true;
        }

        boost::asio::write(socket, boost::asio::buffer(buffer, bytes_read), error);   // <-- blocks here

        if (error)
        {
            std::cout << "[Client] Status: SEND ERROR – " << error.message() << endl;
            break;
        }

        if(bytes_read == 0){
            socket.close();
            std::cout << "[Client] File was sent. Number of bytes: " << total_bytes_transmitted  << endl;
            std::cout << "[Client] Status: DISCONNECTED" << endl;
            break;
		}
    }
}


int main(int argc, char* argv[])
{
    // "role,r" registers both the long form --role and the short form -r.
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help",   "produce help message")
        ("role,r", po::value<string>(), "server or client")
		("file,f", po::value<string>(), "file to send (client only)")
        ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help"))
    {
        std::cout << desc << endl;
        return EXIT_SUCCESS;
    }

    if (!vm.count("role"))
    {
        cerr << "Error: --role / -r argument required (server or client)." << endl;
        return EXIT_FAILURE;
    }

    if (vm["role"].as<string>() == "client" && !vm.count("file"))
    {
        cerr << "Error: --file / -f argument required for client role." << endl;
        return EXIT_FAILURE;
    }
    
    if (vm.count("file") && !vm["file"].empty()) {
        file_to_send = vm["file"].as<std::string>();
    }

    string role = vm["role"].as<string>();
    std::cout << "Role: " << role << endl;

    if      (role == "server") run_server();
    else if (role == "client") run_client();
    else
    {
        cerr << "Unknown role '" << role << "'. Use 'server' or 'client'." << endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
