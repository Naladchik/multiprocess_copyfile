#include <boost/asio.hpp>
#include <boost/program_options.hpp>
#include <iostream>
#include <vector>
#include <cstring>
#include <string>
#include "Message.hpp"
#include "Pipeline.hpp"
#include "ClientStages.hpp"
#include "ServerStages.hpp"

using boost::asio::ip::tcp;
namespace po = boost::program_options;

const int PORT = 12345;
const std::string key_source = "MySuperSecretKeyMustBe32Bytes!!!";
constexpr size_t CHUNK_SIZE = 64 * 1024 - 256; // Leave space for safe packaging headers

// CLIENT EXECUTION FLOW
void run_client(const std::string& filepath) {
    boost::asio::io_context io;
    tcp::resolver resolver(io);
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(PORT));
    tcp::socket socket(io);

    std::cout << "[Client] Connecting to 127.0.0.1:" << PORT << "..." << std::endl;
    boost::asio::connect(socket, endpoints);
    std::cout << "[Client] Connected!" << std::endl;

    // A. Send File Metadata first
    FileInfoMessage meta_msg;
    meta_msg.info.size = std::filesystem::file_size(filepath);
    strncpy_s(meta_msg.info.name, std::filesystem::path(filepath).filename().string().c_str(), sizeof(meta_msg.info.name) - 1);

    std::vector<uint8_t> net_buffer;
    meta_msg.Serialize(net_buffer);
    boost::asio::write(socket, boost::asio::buffer(net_buffer));

    // B. Build the Client Pipeline using Policies
    PipelineStage<FileReaderInner> reader_stage(filepath, CHUNK_SIZE);
    PipelineStage<EncryptionInner> crypto_stage(key_source);

    std::vector<uint8_t> raw_chunk;
    EncryptedChunkMessage enc_chunk_msg;
    std::vector<uint8_t> processed_raw;

    // Message processing flow loop
    while (reader_stage.WaitNextData(raw_chunk)) {
        // Run raw reading step        
        reader_stage.ProcessData(raw_chunk, processed_raw);

        // Run AES cryptographic step
        if (crypto_stage.ProcessData(processed_raw, enc_chunk_msg)) {
            // Serialize and transmit the safe network frame
            enc_chunk_msg.Serialize(net_buffer);

            // Send length prefix so the receiver knows exactly how much to read
            uint32_t packet_size = static_cast<uint32_t>(net_buffer.size());
            boost::asio::write(socket, boost::asio::buffer(&packet_size, sizeof(packet_size)));
            boost::asio::write(socket, boost::asio::buffer(net_buffer));
        }
    }

    reader_stage.NotifyComplete();
    crypto_stage.NotifyComplete();
    std::cout << "[Client] Transmission successfully finished!" << std::endl;
}

// SERVER EXECUTION FLOW
void run_server() {
    boost::asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), PORT));
    std::cout << "[Server] Listening on port " << PORT << "..." << std::endl;

    tcp::socket socket(io);
    acceptor.accept(socket);
    std::cout << "[Server] Connection received from: " << socket.remote_endpoint().address().to_string() << std::endl;

    // A. Receive File Metadata first
    std::vector<uint8_t> recv_buf(sizeof(FileInfoPayload));
    boost::asio::read(socket, boost::asio::buffer(recv_buf));

    FileInfoMessage meta_msg;
    if (!meta_msg.Deserialize(recv_buf)) {
        std::cerr << "[Server] Failed to deserialize metadata." << std::endl;
        return;
    }
    std::cout << "[Server] Metadata received: " << meta_msg.info.name << " (" << meta_msg.info.size << " bytes)" << std::endl;

    // B. Build Server Pipeline
    PipelineStage<DecryptionInner> crypto_stage(key_source);
    PipelineStage<FileWriterInner> writer_stage(meta_msg.info.name);

    uint64_t total_written = 0;
    uint32_t packet_size = 0;
    try {
        while (total_written < meta_msg.info.size) {
            // Read length prefix
            packet_size = 0;
            boost::asio::read(socket, boost::asio::buffer(&packet_size, sizeof(packet_size)));

            // Read the full payload chunk
            recv_buf.resize(packet_size);
            boost::asio::read(socket, boost::asio::buffer(recv_buf));

            EncryptedChunkMessage enc_chunk_msg;
            if (!enc_chunk_msg.Deserialize(recv_buf)) {
                throw std::runtime_error("Corrupted message deserialization.");
            }

            // Execute Decryption Policy
            std::vector<uint8_t> plaintext;
            if (crypto_stage.ProcessData(enc_chunk_msg, plaintext)) {
                // Execute Writer Policy
                bool write_success = false;
                writer_stage.ProcessData(plaintext, write_success);
                if (!write_success) {
                    throw std::runtime_error("Writing output stream crashed.");
                }
                total_written += plaintext.size();
            }
        }

        crypto_stage.NotifyComplete();
        writer_stage.NotifyComplete();
        std::cout << "[Server] Successfully completed saving secure file." << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "[Server Exception] " << e.what() << std::endl;
        writer_stage.GetInner().AbortAndCleanup();
    }
}

int main(int argc, char* argv[]) {
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "produce help message")
        ("role,r", po::value<std::string>(), "server or client")
        ("file,f", po::value<std::string>(), "file to send (client only)");

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        std::cout << desc << std::endl;
        return EXIT_SUCCESS;
    }

    if (!vm.count("role")) {
        std::cerr << "Error: --role / -r is mandatory." << std::endl;
        return EXIT_FAILURE;
    }

    std::string role = vm["role"].as<std::string>();
    if (role == "client") {
        if (!vm.count("file")) {
            std::cerr << "Error: Client role requires path selection via --file." << std::endl;
            return EXIT_FAILURE;
        }
        run_client(vm["file"].as<std::string>());
    }
    else if (role == "server") {
        run_server();
    }
    else {
        std::cerr << "Error: unknown role type." << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}