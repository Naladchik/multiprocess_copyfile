// TCP/IP server-client demo using Boost.Asio (synchronous, educational).
//
// One binary, two roles selected via --role server / --role client (-r).
// The server listens on localhost:12345, accepts one client, and prints
// every line it receives.  The client connects and forwards stdin lines.

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
#include <string>

using namespace std;
using boost::asio::ip::tcp;   // shorthand so we can write tcp::socket etc.
namespace po = boost::program_options;

// The port both sides agree on.  Both must use the same number.
const int PORT = 12345;

// ---------------------------------------------------------------------------
// SERVER
// ---------------------------------------------------------------------------
void run_server()
{
    // io_context is the core Boost.Asio object.
    // All I/O operations go through it.
    boost::asio::io_context io;

    // An acceptor listens on a TCP port and hands out connected sockets.
    // tcp::v4()     – use IPv4
    // PORT          – the port number to bind to
    tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), PORT));

    cout << "[Server] Status: LISTENING on port " << PORT << endl;
    cout << "[Server] Waiting for a client to connect..." << endl;

    // accept() blocks until a client connects.
    // It returns a fully-connected socket we can read/write on.
    tcp::socket socket(io);
    acceptor.accept(socket);   // <-- blocks here

    // If we reach this line, a client has connected.
    // remote_endpoint() tells us the client's IP address and port.
    cout << "[Server] Status: CONNECTED  <-- "
         << socket.remote_endpoint().address().to_string()
         << ":" << socket.remote_endpoint().port() << endl;

    // A streambuf is a resizable byte buffer.
    // read_until() will fill it until it finds the delimiter we specify.
    boost::asio::streambuf buffer;

    // Keep receiving messages until the connection closes or an error occurs.
    while (true)
    {
        boost::system::error_code error;

        // read_until() reads bytes from the socket into 'buffer' until it
        // finds a newline '\n'.  The newline stays in the buffer.
        // We pass 'error' so the function does not throw; instead it sets
        // error to a non-zero value and we check it ourselves.
        boost::asio::read_until(socket, buffer, '\n', error);

        // error == eof means the client closed the connection gracefully.
        if (error == boost::asio::error::eof)
        {
            cout << "[Server] Status: DISCONNECTED (client closed the connection)" << endl;
            break;
        }

        // Any other non-zero error code is unexpected.
        if (error)
        {
            cout << "[Server] Status: ERROR – " << error.message() << endl;
            break;
        }

        // Convert the buffer contents to a std::string for easy printing.
        // istream makes it easy to extract a line from the streambuf.
        istream stream(&buffer);
        string line;
        getline(stream, line);   // extracts up to (and discards) the '\n'

        cout << "[Server] Received: " << line << endl;
    }
}

// ---------------------------------------------------------------------------
// CLIENT
// ---------------------------------------------------------------------------
void run_client()
{
    boost::asio::io_context io;

    // A resolver turns a human-readable address ("127.0.0.1" / "localhost")
    // and service ("12345") into a list of endpoints we can try to connect to.
    tcp::resolver resolver(io);

    // resolve() returns an iterable list of endpoints.
    // "127.0.0.1" is the loopback address – always "this machine".
    auto endpoints = resolver.resolve("127.0.0.1", to_string(PORT));

    tcp::socket socket(io);

    cout << "[Client] Status: CONNECTING to 127.0.0.1:" << PORT << " ..." << endl;

    boost::system::error_code error;

    // connect() tries each endpoint in the list until one succeeds.
    // If all fail it sets 'error'.
    boost::asio::connect(socket, endpoints, error);

    if (error)
    {
        // Could not reach the server – print why and exit.
        cout << "[Client] Status: FAILED to connect – " << error.message() << endl;
        cout << "[Client] Make sure the server is running first." << endl;
        return;
    }

    cout << "[Client] Status: CONNECTED to server" << endl;
    cout << "[Client] Type a message and press Enter to send." << endl;
    cout << "[Client] Type 'quit' to disconnect." << endl;

    string line;

    // Read lines from the keyboard and send them to the server.
    while (true)
    {
        // getline blocks until the user presses Enter.
        getline(cin, line);

        if (line == "quit")
        {
            // Close the socket cleanly.  The server will see EOF and exit.
            socket.close();
            cout << "[Client] Status: DISCONNECTED" << endl;
            break;
        }

        // Append '\n' so the server's read_until('\n') knows where the
        // message ends.  Without it the server would block forever waiting
        // for the delimiter.
        string message = line + "\n";

        // write() sends all bytes of 'message' over the socket.
        // It keeps retrying internally until every byte is sent.
        boost::asio::write(socket, boost::asio::buffer(message), error);

        if (error)
        {
            cout << "[Client] Status: SEND ERROR – " << error.message() << endl;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
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
