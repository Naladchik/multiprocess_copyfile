#pragma once
#include "Pipeline.hpp"
#include "Message.hpp"
#include <fstream>
#include <iostream>
#include <openssl/evp.h>
#include <memory>
#include <stdexcept>
#include <filesystem>

using EVP_CIPHER_CTX_ptr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

// 1. Server Specification for Decryption Inner Class
class DecryptionInner {
private:
    std::array<uint8_t, 32> key_;

public:
    explicit DecryptionInner(const std::string& key_source) {
        std::memcpy(key_.data(), key_source.data(), std::min(key_source.size(), key_.size()));
    }

    bool Wait(const EncryptedChunkMessage& msg) {
        return !msg.payload.empty();
    }

    bool Process(const EncryptedChunkMessage& msg, std::vector<uint8_t>& plaintext) {
        EVP_CIPHER_CTX_ptr ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
        if (!ctx) throw std::runtime_error("Failed to create OpenSSL Decrypt Context.");

        if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
            throw std::runtime_error("Failed to initialize decryption.");
        }

        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(msg.header.iv.size()), nullptr) != 1) {
            throw std::runtime_error("Failed to set decrypt IV length.");
        }

        if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key_.data(), msg.header.iv.data()) != 1) {
            throw std::runtime_error("Failed to set decrypt Key/IV.");
        }

        plaintext.resize(msg.payload.size());
        int out_len = 0;
        if (EVP_DecryptUpdate(ctx.get(), plaintext.data(), &out_len, msg.payload.data(), static_cast<int>(msg.payload.size())) != 1) {
            throw std::runtime_error("Decryption failed during update.");
        }
        int total_len = out_len;

        // Verify the GCM auth tag
        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(msg.header.tag.size()), const_cast<uint8_t*>(msg.header.tag.data())) != 1) {
            throw std::runtime_error("Failed to set expected tag.");
        }

        if (EVP_DecryptFinal_ex(ctx.get(), plaintext.data() + total_len, &out_len) <= 0) {
            throw std::runtime_error("GCM Integrity check failed! Corrupted or modified payload.");
        }

        return true;
    }

    void Complete() {
        std::cout << "[Server Decryptor] Decryption block validation complete." << std::endl;
    }
};

// 2. Server Specification for Writer Inner Class
class FileWriterInner {
private:
    std::ofstream file_;
    std::string filepath_;

public:
    explicit FileWriterInner(const std::string& filename) {
        std::filesystem::create_directories("received");
        filepath_ = "received/" + filename;
        file_.open(filepath_, std::ios::out | std::ios::binary | std::ios::noreplace);
        if (!file_.is_open()) {
            throw std::runtime_error("File already exists or cannot be created: " + filepath_);
        }
    }

    bool Wait(const std::vector<uint8_t>& data) {
        return !data.empty();
    }

    bool Process(const std::vector<uint8_t>& plaintext, bool& success) {
        file_.write(reinterpret_cast<const char*>(plaintext.data()), plaintext.size());
        success = file_.good();
        return success;
    }

    void Complete() {
        if (file_.is_open()) {
            file_.close();
        }
        std::cout << "[Server Writer] File successfully written and saved to " << filepath_ << std::endl;
    }

    void AbortAndCleanup() {
        if (file_.is_open()) {
            file_.close();
        }
        std::filesystem::remove(filepath_);
        std::cout << "[Server Writer] Session aborted. Cleaned up partial file: " << filepath_ << std::endl;
    }
};