// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string_view>

#include "core/file_format/pkg_reader.h"
#include "core/file_sys/ifile.h"

namespace Core::FileSys {

struct PkgSource {
    std::shared_ptr<Pkg::PkgReader> reader;
    Common::FS::FileInfo info{};
    std::filesystem::path path;
};

class PkgFile final : public IFile {
public:
    PkgFile(std::shared_ptr<PkgSource> source, u32 node);
    ~PkgFile() override = default;

    s64 Read(void* dst, u64 size) override;
    bool SupportsReadAt() const override {
        return true;
    }
    s64 ReadAt(void* dst, u64 size, u64 offset) override;
    s64 Write(const void*, u64) override {
        return -1;
    }
    bool Seek(s64 offset, Common::FS::SeekOrigin origin) override;
    u64 Tell() const override;
    u64 Size() const override;
    bool Flush() override {
        return true;
    }
    bool IsOpen() const override {
        return m_source != nullptr;
    }

    std::optional<std::filesystem::path> GetMetadataHostPath() const override {
        return m_source->path;
    }

    void Stat(FileStat& out) override;

    MmapPolicy GetMmapPolicy() const override {
        return MmapPolicy::Copy;
    }
    bool Map(u8* addr, u64 size, u64 offset, u32 raw_prot, const FileMapContext& ctx) override;

    bool IsReadOnly() const override {
        return true;
    }

private:
    std::shared_ptr<PkgSource> m_source;
    u32 m_node;
    mutable std::mutex m_position_mutex;
    u64 m_position{0};
};

class PkgDirectory final : public IDirectory {
public:
    PkgDirectory(std::shared_ptr<PkgSource> source, u32 node)
        : m_source(std::move(source)), m_node(node) {}

    bool Next(DirEntry& out) override;
    void Rewind() override {
        m_index = 0;
    }

private:
    std::shared_ptr<PkgSource> m_source;
    u32 m_node;
    size_t m_index{0};
};

// Backend that serves the files inside a .pkg without extracting it.
class PkgBackend final : public IBackend {
public:
    explicit PkgBackend(const std::filesystem::path& pkg_path);
    ~PkgBackend() override = default;

    bool IsOpen() const {
        return m_source != nullptr;
    }

    bool Exists(std::string_view rel_path) override;
    bool IsDirectory(std::string_view rel_path) override;
    bool Stat(std::string_view rel_path, FileStat& out) override;

    std::unique_ptr<IFile> Open(std::string_view rel_path,
                                Common::FS::FileAccessMode mode) override;
    std::unique_ptr<IDirectory> OpenDir(std::string_view rel_path) override;

    bool IsReadOnly() const override {
        return true;
    }
    std::filesystem::path RootPath() const override {
        return m_path;
    }
    std::optional<std::vector<u8>> ReadFile(std::string_view rel_path) const override;

    const Pkg::PkgReader& Reader() const {
        return *m_source->reader;
    }

private:
    std::filesystem::path m_path;
    std::shared_ptr<PkgSource> m_source;
};

} // namespace Core::FileSys
