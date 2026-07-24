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
#include <vector>
#include <memory>
#include <stdexcept>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <span>

using namespace std;
using boost::asio::ip::tcp;   // shorthand so we can write tcp::socket etc.
namespace po = boost::program_options;
namespace fs = std::filesystem;

const int PORT = 12345;
constexpr uint16_t max_string{ 64 };
constexpr uint16_t kChunkSize{ 0xffff };
constexpr uint16_t kHeaderSize{ 0xff }; //must be not less than KEY_SIZE + IV_SIZE + TAG_SIZE
constexpr size_t KEY_SIZE = 32;       // 256 bits for AES-256
constexpr size_t IV_SIZE = 12;        // 96 bits (standard & recommended for GCM)
constexpr size_t TAG_SIZE = 16;       // 128-bit authentication tag

// Helper unique_ptr cleanup for OpenSSL contexts
using EVP_CIPHER_CTX_ptr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

std::string key_source = "MySuperSecretKeyMustBe32Bytes!!!"; // 32 characters

array<char, kChunkSize> buffer_pdu{};
array<char, kChunkSize - kHeaderSize> buffer_payload{};

struct tEncryptedChunk {
    std::array<uint8_t, IV_SIZE> iv;             // Unique IV used for this chunk
    std::array<uint8_t, TAG_SIZE> tag;           // Authentication tag
};

#pragma pack(push, 1)
struct tFileInfo {
    uint64_t size;
    char name[max_string];
};
#pragma pack(pop)

string file_to_send = "";

void decrypt_chunk(array<char, kChunkSize>& buf_input, array<char, kChunkSize - kHeaderSize>& buf_output, const std::array<uint8_t, KEY_SIZE>& key) {
    void* p = buf_input.data();
    tEncryptedChunk* chunk = static_cast<tEncryptedChunk*>(p);

    std::span<char> data_input{ buf_input.data() + kHeaderSize, kChunkSize - kHeaderSize };

    // 1. Create and initialize Cipher Context
    EVP_CIPHER_CTX_ptr ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx) throw std::runtime_error("Failed to create EVP Cipher Context.");

    // 2. Initialize decryption using AES-256-GCM
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        throw std::runtime_error("Failed to initialize AES-GCM decryption.");
    }

    // 3. Set the IV length
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(chunk->iv.size()), nullptr) != 1) {
        throw std::runtime_error("Failed to set IV length.");
    }

    // 4. Initialize Key and IV
    if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), chunk->iv.data()) != 1) {
        throw std::runtime_error("Failed to set key and IV.");
    }

    // 5. Decrypt ciphertext chunk
    int out_len = 0;
    auto* plaintext_ptr = reinterpret_cast<unsigned char*>(buf_output.data());
    const auto* ciphertext_ptr = reinterpret_cast<const unsigned char*>(data_input.data());

    if (EVP_DecryptUpdate(ctx.get(), plaintext_ptr, &out_len, ciphertext_ptr, static_cast<int>(data_input.size())) != 1) {
        throw std::runtime_error("Decryption failed during update step.");
    }
    int total_len = out_len;

    // 6. Set the expected authentication tag
    // Const cast is required because EVP_CIPHER_CTX_ctrl modifies internal context state, not the tag buffer itself
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(chunk->tag.size()), const_cast<uint8_t*>(chunk->tag.data())) != 1) {
        throw std::runtime_error("Failed to set expected authentication tag.");
    }

    // 7. Finalize decryption. This is where OpenSSL verifies the Tag!
    // If the data was modified, this will fail and return <= 0
    if (EVP_DecryptFinal_ex(ctx.get(), plaintext_ptr + total_len, &out_len) <= 0) {
        throw std::runtime_error("Decryption/Integrity check failed! (Data has been altered or key/IV/tag is incorrect).");
    }

    //std::copy(buf_input.begin() + kHeaderSize, buf_input.end(), buf_output.begin());
}

void encrypt_chunk(const array<char, kChunkSize - kHeaderSize>& buf_input, array<char, kChunkSize>& buf_output, const std::array<uint8_t, KEY_SIZE>& key){
    void* p = buf_output.data();
    tEncryptedChunk* chunk = static_cast<tEncryptedChunk*>(p);

    std::span<char> data_output{ buf_output.data() + kHeaderSize, kChunkSize - kHeaderSize };

    // 1. Generate a cryptographically secure random IV for this chunk
    if (RAND_bytes(chunk->iv.data(), static_cast<int>(chunk->iv.size())) != 1) {
        throw std::runtime_error("Failed to generate random IV.");
    }

    // 2. Create and initialize the OpenSSL Cipher Context
    EVP_CIPHER_CTX_ptr ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx) throw std::runtime_error("Failed to create EVP Cipher Context.");

    // 3. Initialize encryption using AES-256-GCM
    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        throw std::runtime_error("Failed to initialize AES-GCM encryption.");
    }

    // 4. Set the IV length (GCM default is 12, but it's good practice to set it explicitly)
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(chunk->iv.size()), nullptr) != 1) {
        throw std::runtime_error("Failed to set IV length.");
    }

    // 5. Initialize Key and IV
    if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), chunk->iv.data()) != 1) {
        throw std::runtime_error("Failed to set key and IV.");
    }

    // 6. Encrypt the input chunk
    int out_len = 0;
    auto* ciphertext_ptr = reinterpret_cast<unsigned char*>(data_output.data());
    const auto* plaintext_ptr = reinterpret_cast<const unsigned char*>(buf_input.data());

    if (EVP_EncryptUpdate(ctx.get(), ciphertext_ptr, &out_len, plaintext_ptr, static_cast<int>(buf_input.size())) != 1) {
        throw std::runtime_error("Encryption failed during update step.");
    }
    int total_len = out_len;

    // 7. Finalize Encryption
    if (EVP_EncryptFinal_ex(ctx.get(), ciphertext_ptr + total_len, &out_len) != 1) {
        throw std::runtime_error("Encryption failed during final step.");
    }

    // 8. Extract the Authentication Tag (Critical for GCM security)
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(chunk->tag.size()), chunk->tag.data()) != 1) {
        throw std::runtime_error("Failed to retrieve authentication tag.");
    }
    //std::copy(buf_input.begin(), buf_input.end(), buf_output.begin() + kHeaderSize);
}

// ---------------------------------------------------------------------------
// SERVER
// ---------------------------------------------------------------------------
static void run_server()
{
    std::array<uint8_t, KEY_SIZE> secret_key{};
    std::memcpy(secret_key.data(), key_source.data(), KEY_SIZE);

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

    bytes_read = boost::asio::read(socket, boost::asio::buffer(buffer_pdu, size_of_read), error);

    std::cout << "[Server] PDU recieved with size " << bytes_read << " bytes. It is file info." << std::endl;

    if (error)
    {
        std::cout << "[Server] Error during receiving file info. Status: ERROR – " << error.message() << endl;
        return;
    }

	void* p = buffer_pdu.data();
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
        if ((file_size - total_bytes_received) < kChunkSize) {
            size_of_read = file_size - total_bytes_received + kHeaderSize;
        }

        bytes_read = boost::asio::read(socket, boost::asio::buffer(buffer_pdu, size_of_read), error);

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

        std::cout << "[Server] PDU recieved with size " << bytes_read << "bytes. Payload is " << bytes_read - kHeaderSize << " bytes." << std::endl;

        //DECRYPTION HERE
        bytes_read -= kHeaderSize;
        try {
            decrypt_chunk(buffer_pdu, buffer_payload, secret_key);
        }
        catch (const std::exception& e) {
            std::cerr << "Cryptographic Error: " << e.what() << std::endl;
        }

        if (bytes_read > 0) {  
            output_file.write(buffer_payload.data(), bytes_read);
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
    }
}


// ---------------------------------------------------------------------------
// CLIENT
// ---------------------------------------------------------------------------
static void run_client()
{
    std::array<uint8_t, KEY_SIZE> secret_key{};
    std::memcpy(secret_key.data(), key_source.data(), KEY_SIZE);

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
	void* p = buffer_pdu.data();
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

    streamsize bytes_read = sizeof(tFileInfo);

    std::cout << "[Client] transmitting file info of size " << bytes_read << " bytes." << std::endl;
    boost::asio::write(socket, boost::asio::buffer(buffer_pdu, bytes_read), error);

    uint64_t total_bytes_transmitted = 0;  // only file bytes, file info is not counted

    while (true)
    {
        input_file.read(buffer_payload.data(), kChunkSize - kHeaderSize);
        bytes_read = input_file.gcount();
		total_bytes_transmitted += bytes_read;

        if (bytes_read == 0) {
            socket.close();
            std::cout << "[Client] File was sent. Number of bytes: " << total_bytes_transmitted << endl;
            std::cout << "[Client] Status: DISCONNECTED" << endl;
            break;
        }

        //ENCRYPTION HERE
        try {
            encrypt_chunk(buffer_payload, buffer_pdu, secret_key);
        }
        catch (const std::exception& e) {
            std::cerr << "Cryptographic Error: " << e.what() << std::endl;
        }

        std::cout << "[Client] transmitting PDU " << bytes_read + kHeaderSize << " bytes with payload " << bytes_read << " bytes." <<std::endl;

        boost::asio::write(socket, boost::asio::buffer(buffer_pdu, bytes_read + kHeaderSize), error);   // <-- blocks here

        if (error)
        {
            std::cout << "[Client] Status: SEND ERROR – " << error.message() << endl;
            break;
        }
    }
}

bool is_filename_ok(const string& s_input) {
    if (s_input.empty()) return false;
	fs::path p(s_input);
	return !p.has_parent_path() && p != "." && p != ".." && p.filename() == p;
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
        if (!is_filename_ok(file_to_send)) {
            cerr << "Provided file name: " << file_to_send << " is not OK" << endl;
            return EXIT_FAILURE;
        }        
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
