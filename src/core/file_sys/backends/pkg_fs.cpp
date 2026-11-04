// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <limits>

#include "common/logging/log.h"
#include "core/file_sys/backends/pkg_fs.h"

namespace Core::FileSys {

namespace {

bool GetPkgNodeStat(const PkgSource& source, u32 node, FileStat& out) {
    out = {};
    if (node >= std::numeric_limits<u32>::max()) {
        return false;
    }
    const auto inode = GetGuestInode(FileIdentity{
        .kind = FileIdentity::Kind::ArchiveEntry, .host = source.info.identity, .node = node});
    if (!inode || *inode == 0) {
        return false;
    }
    const auto& entry = source.reader->GetNode(node);
    out = {
        .size = entry.is_directory ? 0 : entry.size,
        .inode = *inode,
        .mtime_sec = source.info.mtime_sec,
        .mtime_nsec = source.info.mtime_nsec,
        .atime_sec = source.info.atime_sec,
        .atime_nsec = source.info.atime_nsec,
        .ctime_sec = source.info.ctime_sec,
        .ctime_nsec = source.info.ctime_nsec,
        .is_directory = entry.is_directory,
    };
    return true;
}

} // namespace

PkgFile::PkgFile(std::shared_ptr<PkgSource> source, u32 node)
    : m_source(std::move(source)), m_node(node) {}

s64 PkgFile::ReadAt(void* dst, u64 size, u64 offset) {
    return m_source->reader->Read(m_node, offset, dst, size);
}

s64 PkgFile::Read(void* dst, u64 size) {
    u64 position;
    {
        std::scoped_lock lk{m_position_mutex};
        position = m_position;
    }
    const s64 read = ReadAt(dst, size, position);
    if (read > 0) {
        std::scoped_lock lk{m_position_mutex};
        m_position = position + static_cast<u64>(read);
    }
    return read;
}

bool PkgFile::Seek(s64 offset, Common::FS::SeekOrigin origin) {
    std::scoped_lock lk{m_position_mutex};
    s64 base = 0;
    switch (origin) {
    case Common::FS::SeekOrigin::SetOrigin:
        base = 0;
        break;
    case Common::FS::SeekOrigin::CurrentPosition:
        base = static_cast<s64>(m_position);
        break;
    case Common::FS::SeekOrigin::End:
        base = static_cast<s64>(Size());
        break;
    }
    const s64 target = base + offset;
    if (target < 0) {
        return false;
    }
    m_position = static_cast<u64>(target);
    return true;
}

u64 PkgFile::Tell() const {
    std::scoped_lock lk{m_position_mutex};
    return m_position;
}

u64 PkgFile::Size() const {
    return m_source->reader->GetNode(m_node).size;
}

void PkgFile::Stat(FileStat& out) {
    GetPkgNodeStat(*m_source, m_node, out);
}

bool PkgFile::Map(u8* addr, u64 size, u64 offset, u32 raw_prot, const FileMapContext& ctx) {
    ctx.map_anonymous(addr, size);
    u8* dst = addr;
    u64 remaining = size;
    u64 position = offset;
    while (remaining > 0) {
        const s64 got = ReadAt(dst, remaining, position);
        if (got <= 0) {
            std::memset(dst, 0, remaining);
            break;
        }
        dst += got;
        position += static_cast<u64>(got);
        remaining -= static_cast<u64>(got);
    }
    ctx.protect(addr, size, raw_prot);
    return true;
}

bool PkgDirectory::Next(DirEntry& out) {
    const auto& children = m_source->reader->GetNode(m_node).children;
    if (m_index >= children.size()) {
        return false;
    }
    const auto& child = m_source->reader->GetNode(children[m_index++]);
    out.name = child.name;
    out.is_directory = child.is_directory;
    out.size = child.is_directory ? 0 : child.size;
    return true;
}

PkgBackend::PkgBackend(const std::filesystem::path& pkg_path) : m_path(pkg_path) {
    const auto info = Common::FS::GetFileInfo(pkg_path);
    if (!info) {
        LOG_ERROR(Kernel_Fs, "Failed to open PKG: {}", pkg_path.string());
        return;
    }
    auto reader = std::make_shared<Pkg::PkgReader>();
    std::string error;
    if (!reader->Open(pkg_path, error)) {
        LOG_ERROR(Kernel_Fs, "Failed to open PKG {}: {}", pkg_path.string(), error);
        return;
    }
    m_source = std::make_shared<PkgSource>(PkgSource{std::move(reader), *info, pkg_path});
}

bool PkgBackend::Exists(std::string_view rel_path) {
    return IsOpen() && m_source->reader->Lookup(rel_path) != Pkg::InvalidNode;
}

bool PkgBackend::IsDirectory(std::string_view rel_path) {
    if (!IsOpen()) {
        return false;
    }
    const u32 node = m_source->reader->Lookup(rel_path);
    return node != Pkg::InvalidNode && m_source->reader->GetNode(node).is_directory;
}

bool PkgBackend::Stat(std::string_view rel_path, FileStat& out) {
    if (!IsOpen()) {
        return false;
    }
    const u32 node = m_source->reader->Lookup(rel_path);
    return node != Pkg::InvalidNode && GetPkgNodeStat(*m_source, node, out);
}

std::unique_ptr<IFile> PkgBackend::Open(std::string_view rel_path,
                                        Common::FS::FileAccessMode mode) {
    if (!IsOpen() || mode != Common::FS::FileAccessMode::Read) {
        return nullptr;
    }
    const u32 node = m_source->reader->Lookup(rel_path);
    if (node == Pkg::InvalidNode || m_source->reader->GetNode(node).is_directory) {
        return nullptr;
    }
    return std::make_unique<PkgFile>(m_source, node);
}

std::unique_ptr<IDirectory> PkgBackend::OpenDir(std::string_view rel_path) {
    if (!IsOpen()) {
        return nullptr;
    }
    const u32 node = m_source->reader->Lookup(rel_path);
    if (node == Pkg::InvalidNode || !m_source->reader->GetNode(node).is_directory) {
        return nullptr;
    }
    return std::make_unique<PkgDirectory>(m_source, node);
}

std::optional<std::vector<u8>> PkgBackend::ReadFile(std::string_view rel_path) const {
    if (!IsOpen()) {
        return std::nullopt;
    }
    const u32 node = m_source->reader->Lookup(rel_path);
    if (node == Pkg::InvalidNode || m_source->reader->GetNode(node).is_directory) {
        return std::nullopt;
    }
    std::vector<u8> data(m_source->reader->GetNode(node).size);
    const s64 got = m_source->reader->Read(node, 0, data.data(), data.size());
    if (got < 0) {
        return std::nullopt;
    }
    data.resize(static_cast<size_t>(got));
    return data;
}

} // namespace Core::FileSys
