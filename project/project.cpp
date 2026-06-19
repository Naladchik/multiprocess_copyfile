
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
#include <string>
#include <array>

using namespace std;
using boost::asio::ip::tcp;   // shorthand so we can write tcp::socket etc.
namespace po = boost::program_options;

const int PORT = 12345;

constexpr uint16_t kChunkSize{ 0xffff };


// ---------------------------------------------------------------------------
// SERVER
// ---------------------------------------------------------------------------
void run_server()
{
    boost::asio::io_context io;
    
    tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), PORT));

    ofstream output_file("server_received.txt", ios::binary);

    cout << "[Server] Status: LISTENING on port " << PORT << endl;
    cout << "[Server] Waiting for a client to connect..." << endl;

    tcp::socket socket(io);
    acceptor.accept(socket);   // <-- blocks here

    cout << "[Server] Status: CONNECTED  <-- "
         << socket.remote_endpoint().address().to_string()
         << ":" << socket.remote_endpoint().port() << endl;

    array<char, kChunkSize> buffer;

    while (true)
    {
        boost::system::error_code error;

        size_t bytes_read = socket.read_some(boost::asio::buffer(buffer), error);   // <-- blocks here

        if (bytes_read > 0) {
            output_file.write(buffer.data(), bytes_read);
        }

        if (error == boost::asio::error::eof)
        {
            cout << "[Server] Status: DISCONNECTED (client closed the connection)" << endl;
            break;
        }

        if (error)
        {
            cout << "[Server] Status: ERROR – " << error.message() << endl;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// CLIENT
// ---------------------------------------------------------------------------
void run_client()
{
    boost::asio::io_context io;

    ifstream input_file("input1_initial.txt", ios::binary);

    if (!input_file) {
        cout << "Failed to open source file." << endl;
        return;
    }

    tcp::resolver resolver(io);

    auto endpoints = resolver.resolve("127.0.0.1", to_string(PORT));

    tcp::socket socket(io);

    cout << "[Client] Status: CONNECTING to 127.0.0.1:" << PORT << " ..." << endl;

    boost::system::error_code error;

    boost::asio::connect(socket, endpoints, error);   // <-- blocks here

    if (error)
    {
        cout << "[Client] Status: FAILED to connect – " << error.message() << endl;
        cout << "[Client] Make sure the server is running first." << endl;
        return;
    }

    cout << "[Client] Status: CONNECTED to server" << endl;
    cout << "[Client] Type a message and press Enter to send." << endl;
    cout << "[Client] Type 'quit' to disconnect." << endl;

    array<char, kChunkSize> buffer{ };

    while (true)
    {
        input_file.read(buffer.data(), kChunkSize);
        streamsize bytes_read = input_file.gcount();

        boost::asio::write(socket, boost::asio::buffer(buffer, bytes_read), error);   // <-- blocks here

        if (error)
        {
            cout << "[Client] Status: SEND ERROR – " << error.message() << endl;
            break;
        }

        if(bytes_read == 0){
            socket.close();
            cout << "[Client] Status: DISCONNECTED" << endl;
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
        ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help"))
    {
        cout << desc << endl;
        return 0;
    }

    if (!vm.count("role"))
    {
        cerr << "Error: --role / -r argument required (server or client)." << endl;
        return 1;
    }

    string role = vm["role"].as<string>();
    cout << "Role: " << role << endl;

    if      (role == "server") run_server();
    else if (role == "client") run_client();
    else
    {
        cerr << "Unknown role '" << role << "'. Use 'server' or 'client'." << endl;
        return 1;
    }

    return 0;
}
