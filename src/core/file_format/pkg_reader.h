// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "common/types.h"
#include "core/file_format/pfs.h"

namespace Core::Pkg {

constexpr u32 InvalidNode = 0xFFFFFFFF;

// Read-only view of the files inside a PKG. The PFS image is decrypted and decompressed
// block by block when a file is read, nothing is extracted to disk.
class PkgReader {
public:
    struct Node {
        std::string name;
        std::string path;
        bool is_directory{false};
        u64 size{0};
        u32 inode{0};
        std::vector<u8> data;
        bool has_data{false};
        std::vector<u32> children;
    };

    PkgReader() = default;
    ~PkgReader() = default;

    bool Open(const std::filesystem::path& path, std::string& error);

    u32 Lookup(std::string_view path) const;
    const Node& GetNode(u32 node) const {
        return nodes[node];
    }

    // Reads at an absolute offset in the file, returns the number of bytes read.
    s64 Read(u32 node, u64 offset, void* dst, u64 size);

    const std::string& TitleId() const {
        return title_id;
    }
    const std::string& ContentId() const {
        return content_id;
    }
    u32 ContentFlags() const {
        return content_flags;
    }

private:
    struct PfsFile {
        u32 first_block{0};
        u32 block_count{0};
    };

    struct Keys {
        std::array<u8, 32> dk3{};
        std::array<u8, 32> ekpfs{};
    };

    struct Entry {
        std::array<u8, 32> raw{};
        u32 id{0};
        std::vector<u8> data;
    };

    bool ReadRaw(u64 offset, void* dst, u64 size);
    bool DeriveKeys(const std::vector<Entry>& entries, bool use_rsa, Keys& keys);
    bool IndexPfs(const Keys& keys, std::string& error);
    bool ParsePfsTree(const std::vector<u8>& pfsc, const Keys& keys);
    u32 AddNode(std::string_view path, bool is_directory);
    bool ReadBlock(u32 block, std::vector<u8>& out);
    std::shared_ptr<const std::vector<u8>> GetBlock(u32 block);

    std::ifstream file;
    std::mutex file_mutex;

    std::string title_id;
    std::string content_id;
    u32 content_flags{0};

    u64 pfs_image_offset{0};
    u64 pfsc_offset{0};
    std::array<u8, 16> data_key{};
    std::array<u8, 16> tweak_key{};
    std::vector<u64> sector_map;
    std::vector<Inode> inodes;
    std::unordered_map<u32, PfsFile> pfs_files;
    std::unordered_map<u32, u32> inode_to_node;

    std::vector<Node> nodes;
    std::unordered_map<std::string, u32> path_to_node;

    std::mutex cache_mutex;
    std::list<u32> cache_order;
    struct CachedBlock {
        std::shared_ptr<const std::vector<u8>> data;
        std::list<u32>::iterator position;
    };
    std::unordered_map<u32, CachedBlock> cache;
    static constexpr size_t CacheBlocks = 96;
};

} // namespace Core::Pkg
