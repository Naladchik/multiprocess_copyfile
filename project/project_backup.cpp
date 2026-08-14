#include <iostream>
#include <array>
#include <vector>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <openssl/evp.h>
#include <openssl/rand.h>

constexpr size_t CHUNK_SIZE = 65536;
constexpr size_t KEY_SIZE = 32;       // 256 bits for AES-256
constexpr size_t IV_SIZE = 12;        // 96 bits (standard & recommended for GCM)
constexpr size_t TAG_SIZE = 16;       // 128-bit authentication tag

// Helper unique_ptr cleanup for OpenSSL contexts
using EVP_CIPHER_CTX_ptr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

struct EncryptedChunk {
    std::array<uint8_t, IV_SIZE> iv;             // Unique IV used for this chunk
    std::array<uint8_t, TAG_SIZE> tag;           // Authentication tag
    std::array<char, CHUNK_SIZE> ciphertext;     // Encrypted data (same size as plaintext in GCM)
};

// --- ENCRYPTION SIDE ---
EncryptedChunk encrypt_chunk(const std::array<char, CHUNK_SIZE>& plaintext, const std::array<uint8_t, KEY_SIZE>& key) {
    EncryptedChunk chunk{};

    // 1. Generate a cryptographically secure random IV for this chunk
    if (RAND_bytes(chunk.iv.data(), static_cast<int>(chunk.iv.size())) != 1) {
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
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(chunk.iv.size()), nullptr) != 1) {
        throw std::runtime_error("Failed to set IV length.");
    }

    // 5. Initialize Key and IV
    if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), chunk.iv.data()) != 1) {
        throw std::runtime_error("Failed to set key and IV.");
    }

    // 6. Encrypt the plaintext chunk
    int out_len = 0;
    auto* ciphertext_ptr = reinterpret_cast<unsigned char*>(chunk.ciphertext.data());
    const auto* plaintext_ptr = reinterpret_cast<const unsigned char*>(plaintext.data());

    if (EVP_EncryptUpdate(ctx.get(), ciphertext_ptr, &out_len, plaintext_ptr, static_cast<int>(plaintext.size())) != 1) {
        throw std::runtime_error("Encryption failed during update step.");
    }
    int total_len = out_len;

    // 7. Finalize Encryption
    if (EVP_EncryptFinal_ex(ctx.get(), ciphertext_ptr + total_len, &out_len) != 1) {
        throw std::runtime_error("Encryption failed during final step.");
    }

    // 8. Extract the Authentication Tag (Critical for GCM security)
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(chunk.tag.size()), chunk.tag.data()) != 1) {
        throw std::runtime_error("Failed to retrieve authentication tag.");
    }

    return chunk;
}

// --- DECRYPTION SIDE ---
std::array<char, CHUNK_SIZE> decrypt_chunk(const EncryptedChunk& chunk, const std::array<uint8_t, KEY_SIZE>& key) {
    std::array<char, CHUNK_SIZE> plaintext{};

    // 1. Create and initialize Cipher Context
    EVP_CIPHER_CTX_ptr ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx) throw std::runtime_error("Failed to create EVP Cipher Context.");

    // 2. Initialize decryption using AES-256-GCM
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        throw std::runtime_error("Failed to initialize AES-GCM decryption.");
    }

    // 3. Set the IV length
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(chunk.iv.size()), nullptr) != 1) {
        throw std::runtime_error("Failed to set IV length.");
    }

    // 4. Initialize Key and IV
    if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), chunk.iv.data()) != 1) {
        throw std::runtime_error("Failed to set key and IV.");
    }

    // 5. Decrypt ciphertext chunk
    int out_len = 0;
    auto* plaintext_ptr = reinterpret_cast<unsigned char*>(plaintext.data());
    const auto* ciphertext_ptr = reinterpret_cast<const unsigned char*>(chunk.ciphertext.data());

    if (EVP_DecryptUpdate(ctx.get(), plaintext_ptr, &out_len, ciphertext_ptr, static_cast<int>(chunk.ciphertext.size())) != 1) {
        throw std::runtime_error("Decryption failed during update step.");
    }
    int total_len = out_len;

    // 6. Set the expected authentication tag
    // Const cast is required because EVP_CIPHER_CTX_ctrl modifies internal context state, not the tag buffer itself
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(chunk.tag.size()), const_cast<uint8_t*>(chunk.tag.data())) != 1) {
        throw std::runtime_error("Failed to set expected authentication tag.");
    }

    // 7. Finalize decryption. This is where OpenSSL verifies the Tag!
    // If the data was modified, this will fail and return <= 0
    if (EVP_DecryptFinal_ex(ctx.get(), plaintext_ptr + total_len, &out_len) <= 0) {
        throw std::runtime_error("Decryption/Integrity check failed! (Data has been altered or key/IV/tag is incorrect).");
    }

    return plaintext;
}

int main() {




    try {
        // A. Setup a shared secret key (In real life, share this securely beforehand)
        std::array<uint8_t, KEY_SIZE> secret_key{};
        std::string key_source = "MySuperSecretKeyMustBe32Bytes!!!"; // 32 characters
        std::memcpy(secret_key.data(), key_source.data(), KEY_SIZE);

        // B. Generate sample data inside a 64KB buffer
        std::array<char, CHUNK_SIZE> original_buffer{};
        std::string message = "OpenSSL AES-GCM buffer test. Fast, authentic, secure!";
        std::memcpy(original_buffer.data(), message.data(), message.size());

        std::cout << "--- ENCRYPTION (Side A) ---" << std::endl;
        // Encrypt the chunk
        EncryptedChunk encrypted = encrypt_chunk(original_buffer, secret_key);
        std::cout << "Data Encrypted. IV generated. Tag generated." << std::endl;

        std::cout << "\n--- DECRYPTION (Side B) ---" << std::endl;
        // Decrypt the chunk
        std::array<char, CHUNK_SIZE> decrypted_buffer = decrypt_chunk(encrypted, secret_key);
        std::cout << "Data Decrypted successfully!" << std::endl;

        // Print verified message
        std::cout << "Decrypted Message: " << decrypted_buffer.data() << std::endl;

    }
    catch (const std::exception& e) {
        std::cerr << "Cryptographic Error: " << e.what() << std::endl;
    }

    return 0;
}