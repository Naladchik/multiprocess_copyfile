#pragma once
#include <vector>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

// CRTP interface for message formats
template <typename Derived>
class MessageBase {
public:
    void Serialize(std::vector<uint8_t>& buffer) const {
        static_cast<const Derived*>(this)->SerializeImpl(buffer);
    }

    bool Deserialize(const std::vector<uint8_t>& buffer) {
        return static_cast<Derived*>(this)->DeserializeImpl(buffer);
    }
};

// 1. File Metadata Message
#pragma pack(push, 1)
struct FileInfoPayload {
    uint64_t size;
    char name[64];
};
#pragma pack(pop)

class FileInfoMessage : public MessageBase<FileInfoMessage> {
public:
    FileInfoPayload info{};

    void SerializeImpl(std::vector<uint8_t>& buffer) const {
        buffer.resize(sizeof(FileInfoPayload));
        std::memcpy(buffer.data(), &info, sizeof(FileInfoPayload));
    }

    bool DeserializeImpl(const std::vector<uint8_t>& buffer) {
        if (buffer.size() < sizeof(FileInfoPayload)) return false;
        std::memcpy(&info, buffer.data(), sizeof(FileInfoPayload));
        return true;
    }
};

// 2. Encrypted Data Chunk Message
constexpr size_t IV_SIZE = 12;
constexpr size_t TAG_SIZE = 16;

#pragma pack(push, 1)
struct ChunkHeader {
    std::array<uint8_t, IV_SIZE> iv;
    std::array<uint8_t, TAG_SIZE> tag;
};
#pragma pack(pop)

class EncryptedChunkMessage : public MessageBase<EncryptedChunkMessage> {
public:
    ChunkHeader header{};
    std::vector<uint8_t> payload;

    void SerializeImpl(std::vector<uint8_t>& buffer) const {
        size_t total_size = sizeof(ChunkHeader) + payload.size();
        buffer.resize(total_size);
        std::memcpy(buffer.data(), &header, sizeof(ChunkHeader));
        std::memcpy(buffer.data() + sizeof(ChunkHeader), payload.data(), payload.size());
    }

    bool DeserializeImpl(const std::vector<uint8_t>& buffer) {
        if (buffer.size() < sizeof(ChunkHeader)) return false;
        std::memcpy(&header, buffer.data(), sizeof(ChunkHeader));
        payload.resize(buffer.size() - sizeof(ChunkHeader));
        std::memcpy(payload.data(), buffer.data() + sizeof(ChunkHeader), payload.size());
        return true;
    }
};