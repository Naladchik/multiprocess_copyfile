#pragma once
#include "Pipeline.hpp"
#include "Message.hpp"
#include <fstream>
#include <iostream>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdexcept>
#include <memory>

using EVP_CIPHER_CTX_ptr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

// 1. Client Specification for Reader Inner Class
class FileReaderInner {
private:
    std::ifstream file_;
    size_t chunk_size_;

public:
    FileReaderInner(const std::string& filepath, size_t chunk_size)
        : file_(filepath, std::ios::binary), chunk_size_(chunk_size) {
        if (!file_) {
            throw std::runtime_error("Failed to open source file: " + filepath);
        }
    }

    bool Wait(std::vector<uint8_t>& data) {
        data.resize(chunk_size_);
        file_.read(reinterpret_cast<char*>(data.data()), chunk_size_);
        size_t read_bytes = file_.gcount();
        data.resize(read_bytes);
        return read_bytes > 0;
    }

    bool Process(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
        output = input; // Reader pass-through
        return true;
    }

    void Complete() {
        if (file_.is_open()) {
            file_.close();
        }
        std::cout << "[Client Reader] Completed reading file." << std::endl;
    }
};

// 2. Client Specification for Encryption Inner Class
class EncryptionInner {
private:
    std::array<uint8_t, 32> key_;

public:
    explicit EncryptionInner(const std::string& key_source) {
        std::memcpy(key_.data(), key_source.data(), std::min(key_source.size(), key_.size()));
    }

    bool Wait(const std::vector<uint8_t>& data) {
        return !data.empty();
    }

    bool Process(const std::vector<uint8_t>& plaintext, EncryptedChunkMessage& output) {
        // Generate cryptographic random IV
        if (RAND_bytes(output.header.iv.data(), static_cast<int>(output.header.iv.size())) != 1) {
            throw std::runtime_error("Failed to generate random IV.");
        }

        EVP_CIPHER_CTX_ptr ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
        if (!ctx) throw std::runtime_error("Failed to create OpenSSL EVP context.");

        if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
            throw std::runtime_error("Failed to initialize AES-GCM.");
        }

        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(output.header.iv.size()), nullptr) != 1) {
            throw std::runtime_error("Failed to set IV length.");
        }

        if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key_.data(), output.header.iv.data()) != 1) {
            throw std::runtime_error("Failed to set encryption Key/IV.");
        }

        output.payload.resize(plaintext.size());
        int out_len = 0;
        if (EVP_EncryptUpdate(ctx.get(), output.payload.data(), &out_len, plaintext.data(), static_cast<int>(plaintext.size())) != 1) {
            throw std::runtime_error("Encryption failed during update step.");
        }
        int total_len = out_len;

        if (EVP_EncryptFinal_ex(ctx.get(), output.payload.data() + total_len, &out_len) != 1) {
            throw std::runtime_error("Encryption failed during final step.");
        }

        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(output.header.tag.size()), output.header.tag.data()) != 1) {
            throw std::runtime_error("Failed to retrieve authentication tag.");
        }

        return true;
    }

    void Complete() {
        std::cout << "[Client Encryptor] Encryption complete." << std::endl;
    }
};