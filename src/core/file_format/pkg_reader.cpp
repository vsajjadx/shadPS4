// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rsa.h>

#ifdef PKG_READER_USE_ZLIB
#include <zlib.h>
#else
#include <miniz.h>
#endif

#include "common/logging/log.h"
#include "core/file_format/pkg_keys.h"
#include "core/file_format/pkg_reader.h"
#include "core/file_format/pkg_type.h"

namespace Core::Pkg {

namespace {

constexpr u32 PfscMagic = 0x43534650;
constexpr u64 BlockSize = 0x10000;
constexpr u64 SectorSize = 0x1000;
constexpr std::string_view DefaultPasscode = "00000000000000000000000000000000";

u32 BE32(const u8* p) {
    return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | u32{p[3]};
}

u64 BE64(const u8* p) {
    return (u64{BE32(p)} << 32) | BE32(p + 4);
}

bool Inflate(const u8* src, u64 src_size, u8* dst, u64 dst_size) {
#ifdef PKG_READER_USE_ZLIB
    uLongf out = static_cast<uLongf>(dst_size);
    return uncompress(dst, &out, src, static_cast<uLong>(src_size)) == Z_OK;
#else
    mz_ulong out = static_cast<mz_ulong>(dst_size);
    return mz_uncompress(dst, &out, src, static_cast<mz_ulong>(src_size)) == MZ_OK;
#endif
}

void Sha256(const u8* data, size_t size, u8* out) {
    unsigned int length = 0;
    EVP_Digest(data, size, out, &length, EVP_sha256(), nullptr);
}

template <typename Keyset>
bool RsaDecrypt(const u8* cipher, u8* out32) {
    RSA* rsa = RSA_new();
    const auto bn = [](const auto& bytes) {
        return BN_bin2bn(bytes, static_cast<int>(sizeof(bytes)), nullptr);
    };
    RSA_set0_key(rsa, bn(Keyset::Modulus), bn(Keyset::PublicExponent), bn(Keyset::PrivateExponent));
    RSA_set0_factors(rsa, bn(Keyset::Prime1), bn(Keyset::Prime2));
    RSA_set0_crt_params(rsa, bn(Keyset::Exponent1), bn(Keyset::Exponent2), bn(Keyset::Coefficient));
    std::array<u8, 256> plain{};
    const int length = RSA_private_decrypt(256, cipher, plain.data(), rsa, RSA_PKCS1_PADDING);
    RSA_free(rsa);
    if (length < 32) {
        return false;
    }
    std::memcpy(out32, plain.data(), 32);
    return true;
}

bool AesCbcDecrypt(const std::array<u8, 32>& iv_key, const u8* src, u8* dst, size_t size) {
    size -= size % 16;
    if (size == 0) {
        return true;
    }
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int out = 0;
    bool ok =
        EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, iv_key.data() + 16, iv_key.data()) == 1;
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    ok = ok && EVP_DecryptUpdate(ctx, dst, &out, src, static_cast<int>(size)) == 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

void PasscodeKey(std::string_view content_id, u32 index, u8* key) {
    std::array<u8, 96> data{};
    const std::array<u8, 4> index_be{static_cast<u8>(index >> 24), static_cast<u8>(index >> 16),
                                     static_cast<u8>(index >> 8), static_cast<u8>(index)};
    Sha256(index_be.data(), index_be.size(), data.data());
    std::array<u8, 48> cid{};
    std::memcpy(cid.data(), content_id.data(), std::min<size_t>(content_id.size(), 36));
    Sha256(cid.data(), cid.size(), data.data() + 32);
    std::memcpy(data.data() + 64, DefaultPasscode.data(), 32);
    Sha256(data.data(), data.size(), key);
}

void XorTweaks(u8* data, const std::array<u64, SectorSize / 8>& tweaks) {
    for (size_t i = 0; i < tweaks.size(); ++i) {
        u64 word;
        std::memcpy(&word, data + i * 8, 8);
        word ^= tweaks[i];
        std::memcpy(data + i * 8, &word, 8);
    }
}

void XtsDecrypt(const std::array<u8, 16>& data_key, const std::array<u8, 16>& tweak_key,
                const u8* src, u8* dst, size_t size, u64 first_sector) {
    const EVP_CIPHER* const ecb = EVP_aes_128_ecb();
    EVP_CIPHER_CTX* decrypt = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX* tweak = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(decrypt, ecb, nullptr, data_key.data(), nullptr);
    EVP_EncryptInit_ex(tweak, ecb, nullptr, tweak_key.data(), nullptr);
    EVP_CIPHER_CTX_set_padding(decrypt, 0);
    EVP_CIPHER_CTX_set_padding(tweak, 0);
    std::array<u64, SectorSize / 8> tweaks;
    for (size_t offset = 0; offset + SectorSize <= size; offset += SectorSize) {
        std::array<u64, 2> t{first_sector + offset / SectorSize, 0};
        int out = 0;
        EVP_EncryptUpdate(tweak, reinterpret_cast<u8*>(t.data()), &out,
                          reinterpret_cast<const u8*>(t.data()), 16);
        for (size_t i = 0; i < tweaks.size(); i += 2) {
            tweaks[i] = t[0];
            tweaks[i + 1] = t[1];
            const u64 carry = t[1] >> 63;
            t[1] = (t[1] << 1) | (t[0] >> 63);
            t[0] = (t[0] << 1) ^ (carry * 0x87);
        }
        u8* sector = dst + offset;
        std::memcpy(sector, src + offset, SectorSize);
        XorTweaks(sector, tweaks);
        EVP_DecryptUpdate(decrypt, sector, &out, sector, static_cast<int>(SectorSize));
        XorTweaks(sector, tweaks);
    }
    EVP_CIPHER_CTX_free(tweak);
    EVP_CIPHER_CTX_free(decrypt);
}

void PfsGenCryptoKey(const std::array<u8, 32>& ekpfs, const std::array<u8, 16>& seed,
                     std::array<u8, 16>& data_key, std::array<u8, 16>& tweak_key) {
    std::array<u8, 20> d{};
    const u32 index = 1;
    std::memcpy(d.data(), &index, sizeof(index));
    std::memcpy(d.data() + 4, seed.data(), seed.size());
    std::array<u8, 32> digest{};
    unsigned int length = 0;
    HMAC(EVP_sha256(), ekpfs.data(), static_cast<int>(ekpfs.size()), d.data(), d.size(),
         digest.data(), &length);
    std::memcpy(tweak_key.data(), digest.data(), 16);
    std::memcpy(data_key.data(), digest.data() + 16, 16);
}

std::string JoinPath(const std::string& base, std::string_view name) {
    if (base.empty()) {
        return std::string{name};
    }
    return base + "/" + std::string{name};
}

} // namespace

bool PkgReader::ReadRaw(u64 offset, void* dst, u64 size) {
    std::scoped_lock lock{file_mutex};
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(static_cast<char*>(dst), static_cast<std::streamsize>(size));
    const auto got = static_cast<u64>(file.gcount());
    if (got < size) {
        std::memset(static_cast<u8*>(dst) + got, 0, size - got);
    }
    return got > 0 || size == 0;
}

u32 PkgReader::AddNode(std::string_view path, bool is_directory) {
    if (const auto it = path_to_node.find(std::string{path}); it != path_to_node.end()) {
        auto& existing = nodes[it->second];
        if (is_directory == existing.is_directory) {
            return it->second;
        }
        existing.is_directory = is_directory;
        existing.has_data = false;
        existing.data.clear();
        return it->second;
    }
    u32 parent = 0;
    std::string name{path};
    if (const auto slash = path.rfind('/'); slash != std::string_view::npos) {
        parent = AddNode(path.substr(0, slash), true);
        name = std::string{path.substr(slash + 1)};
    }
    const u32 index = static_cast<u32>(nodes.size());
    auto& node = nodes.emplace_back();
    node.name = std::move(name);
    node.path = std::string{path};
    node.is_directory = is_directory;
    nodes[parent].children.push_back(index);
    path_to_node.emplace(node.path, index);
    return index;
}

u32 PkgReader::Lookup(std::string_view path) const {
    while (!path.empty() && path.front() == '/') {
        path.remove_prefix(1);
    }
    while (!path.empty() && path.back() == '/') {
        path.remove_suffix(1);
    }
    if (path.empty()) {
        return 0;
    }
    const auto it = path_to_node.find(std::string{path});
    return it == path_to_node.end() ? InvalidNode : it->second;
}

bool PkgReader::DeriveKeys(const std::vector<Entry>& entries, bool use_rsa, Keys& keys) {
    const Entry* entry_keys = nullptr;
    const Entry* image_key = nullptr;
    for (const auto& entry : entries) {
        if (entry.id == 0x10) {
            entry_keys = &entry;
        } else if (entry.id == 0x20) {
            image_key = &entry;
        }
    }
    if (!entry_keys || !image_key || image_key->data.size() < 256 ||
        entry_keys->data.size() < 32 + 7 * 32 + 7 * 256) {
        return false;
    }
    const u8* key3 = entry_keys->data.data() + 32 + 7 * 32 + 3 * 256;
    if (use_rsa) {
        if (!RsaDecrypt<PkgDerivedKey3Keyset>(key3, keys.dk3.data())) {
            return false;
        }
        std::array<u8, 64> concatenated{};
        std::memcpy(concatenated.data(), image_key->raw.data(), 24);
        std::memcpy(concatenated.data() + 32, keys.dk3.data(), 32);
        std::array<u8, 32> iv_key{};
        Sha256(concatenated.data(), concatenated.size(), iv_key.data());
        std::array<u8, 256> image_plain{};
        if (!AesCbcDecrypt(iv_key, image_key->data.data(), image_plain.data(), 256)) {
            return false;
        }
        return RsaDecrypt<FakeKeyset>(image_plain.data(), keys.ekpfs.data());
    }
    PasscodeKey(content_id, 3, keys.dk3.data());
    PasscodeKey(content_id, 1, keys.ekpfs.data());
    return true;
}

bool PkgReader::PeekContentId(const std::filesystem::path& path, std::string& content_id) {
    std::ifstream pkg(path, std::ios::binary);
    std::array<u8, 0x64> header{};
    if (!pkg.read(reinterpret_cast<char*>(header.data()), header.size()) ||
        BE32(header.data()) != 0x7F434E54) {
        return false;
    }
    content_id.assign(reinterpret_cast<const char*>(header.data() + 0x40), 0x24);
    content_id.resize(std::strlen(content_id.c_str()));
    return true;
}

bool PkgReader::Open(const std::filesystem::path& path, std::string& error) {
    file.open(path, std::ios::binary);
    if (!file.is_open()) {
        error = "Could not open the PKG file";
        return false;
    }
    file.seekg(0, std::ios::end);
    const u64 file_size = static_cast<u64>(file.tellg());

    std::array<u8, 0x440> header{};
    ReadRaw(0, header.data(), header.size());
    if (BE32(header.data()) != 0x7F434E54) {
        error = "File doesn't appear to be a valid PKG file";
        return false;
    }
    const u32 table_entries = BE32(header.data() + 0x10);
    const u32 table_offset = BE32(header.data() + 0x18);
    const u64 content_offset = BE64(header.data() + 0x30);
    const u64 content_size = BE64(header.data() + 0x38);
    content_flags = BE32(header.data() + 0x78);
    pfs_image_offset = BE64(header.data() + 0x410);
    const u64 pkg_size = BE64(header.data() + 0x430);
    const u32 pfs_cache_size = BE32(header.data() + 0x43C);
    content_id.assign(reinterpret_cast<const char*>(header.data() + 0x40), 0x24);
    content_id.resize(std::strlen(content_id.c_str()));
    if (content_id.size() >= 16) {
        title_id = content_id.substr(7, 9);
    }
    if (pkg_size > file_size) {
        error = "PKG file size is different";
        return false;
    }
    if (content_size + content_offset > pkg_size) {
        error = "Content size is bigger than pkg size";
        return false;
    }

    std::vector<Entry> entries;
    for (u32 i = 0; i < table_entries; ++i) {
        Entry entry;
        ReadRaw(static_cast<u64>(table_offset) + i * 32ull, entry.raw.data(), 24);
        entry.id = BE32(entry.raw.data());
        const u32 offset = BE32(entry.raw.data() + 16);
        const u32 size = BE32(entry.raw.data() + 20);
        entry.data.resize(size);
        if (size > 0 && !ReadRaw(offset, entry.data.data(), size)) {
            error = "Failed to read a PKG entry";
            return false;
        }
        entries.push_back(std::move(entry));
    }

    Keys keys;
    bool indexed = false;
    for (const bool use_rsa : {true, false}) {
        keys = {};
        nodes.clear();
        path_to_node.clear();
        pfs_files.clear();
        nodes.emplace_back().is_directory = true;
        path_to_node.emplace("", 0);
        if (!DeriveKeys(entries, use_rsa, keys)) {
            continue;
        }
        std::array<u8, 16> seed{};
        ReadRaw(pfs_image_offset + 0x370, seed.data(), seed.size());
        PfsGenCryptoKey(keys.ekpfs, seed, data_key, tweak_key);
        std::string index_error;
        pfsc_offset = 0;
        sector_map.clear();
        inodes.clear();
        if (pfs_cache_size == 0) {
            indexed = true;
            break;
        }
        const u64 length = static_cast<u64>(pfs_cache_size) * 2;
        std::vector<u8> encrypted(length);
        ReadRaw(pfs_image_offset, encrypted.data(), length);
        std::vector<u8> decrypted(length);
        XtsDecrypt(data_key, tweak_key, encrypted.data(), decrypted.data(), length, 0);
        u64 found = InvalidNode;
        for (u64 at = 0x20000; at + 4 <= length; at += 0x10000) {
            u32 magic;
            std::memcpy(&magic, decrypted.data() + at, sizeof(magic));
            if (magic == PfscMagic) {
                found = at;
                break;
            }
        }
        if (found == InvalidNode) {
            continue;
        }
        pfsc_offset = found;
        std::vector<u8> pfsc(length);
        std::memcpy(pfsc.data(), decrypted.data() + found, length - found);
        if (ParsePfsTree(pfsc, keys)) {
            indexed = true;
            break;
        }
    }
    if (!indexed) {
        error = "The game data could not be decrypted with the built-in keys.";
        return false;
    }

    for (auto& entry : entries) {
        const auto name = GetEntryNameByType(entry.id);
        const std::string rel =
            "sce_sys/" + (name.empty() ? std::to_string(entry.id) : std::string{name});
        if (entry.id >= 0x400 && entry.id <= 0x403) {
            std::array<u8, 64> concatenated{};
            std::memcpy(concatenated.data(), entry.raw.data(), 24);
            std::memcpy(concatenated.data() + 32, keys.dk3.data(), 32);
            std::array<u8, 32> iv_key{};
            Sha256(concatenated.data(), concatenated.size(), iv_key.data());
            std::vector<u8> plain = entry.data;
            AesCbcDecrypt(iv_key, entry.data.data(), plain.data(), entry.data.size());
            entry.data = std::move(plain);
        }
        const u32 index = AddNode(rel, false);
        auto& node = nodes[index];
        if (node.is_directory) {
            continue;
        }
        node.size = entry.data.size();
        node.data = std::move(entry.data);
        node.has_data = true;
    }
    return true;
}

bool PkgReader::ParsePfsTree(const std::vector<u8>& pfsc, const Keys&) {
    PFSCHdr header;
    std::memcpy(&header, pfsc.data(), sizeof(header));
    if (header.block_sz2 <= 0) {
        return false;
    }
    const s64 num_blocks = header.data_length / header.block_sz2;
    sector_map.assign(static_cast<size_t>(num_blocks) + 1, 0);
    for (s64 i = 0; i < num_blocks + 1; ++i) {
        const u64 at = static_cast<u64>(header.block_offsets) + static_cast<u64>(i) * 8;
        if (at + 8 > pfsc.size()) {
            sector_map.resize(static_cast<size_t>(i));
            break;
        }
        std::memcpy(&sector_map[static_cast<size_t>(i)], pfsc.data() + at, 8);
    }

    std::vector<u8> block(BlockSize);
    size_t diag_blocks_done = 0;
    const char* diag_stop = "all blocks";
    bool diag_end_reached = false;
    s32 entry_size = 0;
    u32 inode_total = 0;
    s32 inode_counter = 0;
    bool dinode_reached = false;
    bool uroot_reached = false;
    bool found_root = false;
    std::unordered_map<s32, std::string> paths;
    std::string current_dir;

    for (size_t i = 0; i + 1 < sector_map.size(); ++i) {
        const u64 sector_offset = sector_map[i];
        const u64 sector_size = sector_map[i + 1] - sector_offset;
        if (sector_offset + sector_size > pfsc.size() || sector_size > BlockSize) {
            diag_stop = "sector outside decrypted cache";
            break;
        }
        ++diag_blocks_done;
        if (sector_size == BlockSize) {
            std::memcpy(block.data(), pfsc.data() + sector_offset, BlockSize);
        } else if (!Inflate(pfsc.data() + sector_offset, sector_size, block.data(), BlockSize)) {
            return false;
        }

        if (i == 0) {
            std::memcpy(&inode_total, block.data() + 0x30, 4);
        }
        u32 occupied_blocks = (inode_total * 0xA8) / 0x10000;
        if (((inode_total * 0xA8) % 0x10000) != 0) {
            ++occupied_blocks;
        }
        if (i >= 1 && i <= occupied_blocks) {
            for (u64 p = 0; p + sizeof(Inode) <= BlockSize; p += 0xA8) {
                Inode node;
                std::memcpy(&node, block.data() + p, sizeof(node));
                if (node.Mode == 0) {
                    break;
                }
                inodes.push_back(node);
            }
        }

        if (std::string_view{reinterpret_cast<const char*>(block.data()) + 0x10, 15} ==
            "flat_path_table") {
            uroot_reached = true;
        }
        if (uroot_reached) {
            for (u64 at = 0; at < BlockSize; at += static_cast<u64>(entry_size)) {
                Dirent dirent{};
                std::memcpy(&dirent, block.data() + at,
                            std::min<u64>(sizeof(dirent), BlockSize - at));
                entry_size = dirent.entsize;
                if (dirent.ino != 0) {
                    ++inode_counter;
                } else {
                    paths[inode_counter] = "";
                    found_root = true;
                    uroot_reached = false;
                    break;
                }
                if (entry_size <= 0) {
                    break;
                }
            }
        }

        if (block[0x10] == '.' &&
            std::string_view{reinterpret_cast<const char*>(block.data()) + 0x28, 2} == "..") {
            dinode_reached = true;
        }

        bool end_reached = false;
        if (dinode_reached) {
            for (u64 at = 0; at < BlockSize; at += static_cast<u64>(entry_size)) {
                Dirent dirent{};
                std::memcpy(&dirent, block.data() + at,
                            std::min<u64>(sizeof(dirent), BlockSize - at));
                if (dirent.ino == 0) {
                    break;
                }
                entry_size = dirent.entsize;
                const std::string_view name{dirent.name,
                                            static_cast<size_t>(std::max(dirent.namelen, 0))};
                if (dirent.type == PFS_CURRENT_DIR) {
                    current_dir = paths[dirent.ino];
                    paths[dirent.ino] = current_dir;
                } else if (dirent.type != PFS_PARENT_DIR) {
                    paths[dirent.ino] = JoinPath(current_dir, name);
                }

                if (dirent.type == PFS_FILE || dirent.type == PFS_DIR) {
                    const bool is_dir = dirent.type == PFS_DIR;
                    const u32 index = AddNode(paths[dirent.ino], is_dir);
                    auto& node = nodes[index];
                    if (!is_dir && static_cast<size_t>(dirent.ino) < inodes.size()) {
                        const Inode& inode = inodes[dirent.ino];
                        node.inode = static_cast<u32>(dirent.ino);
                        node.size = static_cast<u64>(inode.Size);
                        node.has_data = false;
                        pfs_files[node.inode] = PfsFile{inode.loc, inode.Blocks};
                    }
                    ++inode_counter;
                    if (static_cast<u32>(inode_counter + 1) == inode_total) {
                        end_reached = true;
                    }
                }
                if (entry_size <= 0) {
                    break;
                }
            }
        }
        if (end_reached) {
            diag_end_reached = true;
            diag_stop = "last inode reached";
            break;
        }
    }
    size_t diag_dirs = 0;
    for (const auto& n : nodes) {
        diag_dirs += n.is_directory ? 1 : 0;
    }
    LOG_INFO(Kernel_Fs,
             "PKG tree: inodes={} inode_counter={} nodes={} dirs={} files={} blocks_parsed={}/{} "
             "cache_bytes={} stop='{}' end_reached={}",
             inode_total, inode_counter, nodes.size(), diag_dirs, nodes.size() - diag_dirs,
             diag_blocks_done, sector_map.size() > 0 ? sector_map.size() - 1 : 0, pfsc.size(),
             diag_stop, diag_end_reached);
    // Diagnostics only: list the directories with their child counts (a PKG holds few of them).
    size_t diag_listed = 0;
    for (const auto& n : nodes) {
        if (n.is_directory && diag_listed++ < 64) {
            LOG_INFO(Kernel_Fs, "PKG dir: '{}' children={}", n.path, n.children.size());
        }
    }
    return found_root;
}

PkgReader::BlockSpan PkgReader::GetBlockSpan(u32 index) const {
    const u64 sector_offset = sector_map[index];
    const u64 absolute = pfsc_offset + sector_offset;
    BlockSpan span;
    span.size = sector_map[index + 1] - sector_offset;
    span.aligned = absolute & ~(SectorSize - 1);
    span.skip = absolute - span.aligned;
    span.length = (span.skip + span.size + SectorSize - 1) & ~(SectorSize - 1);
    return span;
}

bool PkgReader::DecodeBlock(u32 index, const u8* encrypted, u8* out) const {
    const BlockSpan span = GetBlockSpan(index);
    if (span.size > BlockSize) {
        return false;
    }
    thread_local std::vector<u8> decrypted;
    decrypted.resize(span.length);
    XtsDecrypt(data_key, tweak_key, encrypted, decrypted.data(), span.length,
               span.aligned / SectorSize);
    if (span.size == BlockSize) {
        std::memcpy(out, decrypted.data() + span.skip, BlockSize);
        return true;
    }
    if (span.size == 0) {
        std::memset(out, 0, BlockSize);
        return true;
    }
    return Inflate(decrypted.data() + span.skip, span.size, out, BlockSize);
}

bool PkgReader::ReadBlock(u32 index, std::vector<u8>& out) {
    if (static_cast<size_t>(index) + 1 >= sector_map.size()) {
        return false;
    }
    const BlockSpan span = GetBlockSpan(index);
    if (span.size > BlockSize) {
        return false;
    }
    std::vector<u8> encrypted(span.length);
    ReadRaw(pfs_image_offset + span.aligned, encrypted.data(), span.length);
    out.resize(BlockSize);
    return DecodeBlock(index, encrypted.data(), out.data());
}

bool PkgReader::ReadBlocks(u32 first, u32 count, u64 skip, u64 size, u8* dst) {
    if (static_cast<size_t>(first) + count >= sector_map.size()) {
        return false;
    }
    const u64 start = GetBlockSpan(first).aligned;
    const BlockSpan last = GetBlockSpan(first + count - 1);
    std::vector<u8> encrypted(last.aligned + last.length - start);
    ReadRaw(pfs_image_offset + start, encrypted.data(), encrypted.size());

    const u32 workers = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    std::atomic<u32> next{0};
    std::atomic<bool> ok{true};
    const auto work = [&] {
        std::vector<u8> partial(BlockSize);
        for (u32 i = next++; i < count; i = next++) {
            const u32 index = first + i;
            const u8* source = encrypted.data() + (GetBlockSpan(index).aligned - start);
            const u64 begin = i == 0 ? skip : 0;
            const u64 block_start = static_cast<u64>(i) * BlockSize;
            const u64 end = std::min<u64>(BlockSize, skip + size - block_start);
            u8* target = dst + block_start + begin - skip;
            if (begin == 0 && end == BlockSize) {
                ok = DecodeBlock(index, source, target) && ok;
            } else if (DecodeBlock(index, source, partial.data())) {
                std::memcpy(target, partial.data() + begin, end - begin);
            } else {
                ok = false;
            }
        }
    };
    std::vector<std::jthread> threads;
    for (u32 i = 1; i < std::min(workers, count); ++i) {
        threads.emplace_back(work);
    }
    work();
    threads.clear();
    return ok;
}

std::shared_ptr<const std::vector<u8>> PkgReader::GetBlock(u32 index) {
    {
        std::scoped_lock lock{cache_mutex};
        if (const auto it = cache.find(index); it != cache.end()) {
            cache_order.splice(cache_order.begin(), cache_order, it->second.position);
            return it->second.data;
        }
    }
    auto block = std::make_shared<std::vector<u8>>();
    if (!ReadBlock(index, *block)) {
        return nullptr;
    }
    std::scoped_lock lock{cache_mutex};
    if (const auto it = cache.find(index); it != cache.end()) {
        return it->second.data;
    }
    cache_order.push_front(index);
    cache.emplace(index, CachedBlock{block, cache_order.begin()});
    if (cache.size() > CacheBlocks) {
        cache.erase(cache_order.back());
        cache_order.pop_back();
    }
    return block;
}

s64 PkgReader::Read(u32 node_index, u64 offset, void* dst, u64 size) {
    if (node_index >= nodes.size()) {
        return -1;
    }
    const Node& node = nodes[node_index];
    if (node.is_directory || offset >= node.size) {
        return 0;
    }
    size = std::min(size, node.size - offset);
    if (node.has_data) {
        std::memcpy(dst, node.data.data() + offset, size);
        return static_cast<s64>(size);
    }
    const auto it = pfs_files.find(node.inode);
    if (it == pfs_files.end()) {
        return -1;
    }
    auto* out = static_cast<u8*>(dst);
    u64 done = 0;
    while (size - done >= BulkReadBlocks * BlockSize) {
        const u64 position = offset + done;
        const u64 block_index = position / BlockSize;
        const u64 in_block = position % BlockSize;
        const u64 count = std::min<u64>((in_block + size - done) / BlockSize, MaxBulkBlocks);
        if (block_index + count > it->second.block_count) {
            break;
        }
        const u64 chunk = count * BlockSize - in_block;
        if (!ReadBlocks(it->second.first_block + static_cast<u32>(block_index),
                        static_cast<u32>(count), in_block, chunk, out + done)) {
            return done > 0 ? static_cast<s64>(done) : -1;
        }
        done += chunk;
    }
    while (done < size) {
        const u64 position = offset + done;
        const u64 block_index = position / BlockSize;
        if (block_index >= it->second.block_count) {
            break;
        }
        const auto block = GetBlock(it->second.first_block + static_cast<u32>(block_index));
        if (!block) {
            return done > 0 ? static_cast<s64>(done) : -1;
        }
        const u64 in_block = position % BlockSize;
        const u64 chunk = std::min(size - done, BlockSize - in_block);
        std::memcpy(out + done, block->data() + in_block, chunk);
        done += chunk;
    }
    return static_cast<s64>(done);
}

} // namespace Core::Pkg
