// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/scm_rev.h"
#include "common/thread.h"
#include "core/emulator_settings.h"

#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <system_error>
#include <vector>

namespace {

std::condition_variable_any request_cv{};
std::queue<std::packaged_task<void()>> req_queue{};
std::mutex m_request{};

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");

    // Requests still queued when a stop is requested are written before exiting, otherwise
    // every pipeline compiled shortly before shutdown would be missing from the next warm-up.
    while (true) {
        std::packaged_task<void()> request{};
        {
            std::unique_lock lk{m_request};
            Common::CondvarWait(request_cv, lk, stoken, [&] { return !req_queue.empty(); });
            if (req_queue.empty()) {
                break;
            }
            request = std::move(req_queue.front());
            req_queue.pop();
        }
        if (request.valid()) {
            request();
        }
    }
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    default:
        UNREACHABLE();
    }
}

// Caches used to be stored directly in the game's folder. Move one made by this build into the
// build's own folder; one made by another build is moved into a folder named after that build.
static void MigrateFlatCache(const std::filesystem::path& game_dir,
                             const std::filesystem::path& build_dir,
                             const std::string& build_hash) {
    std::error_code ec{};
    const auto build_file = game_dir / "build.bin";
    if (!std::filesystem::is_regular_file(build_file, ec)) {
        return;
    }
    std::string cached_build;
    {
        const Common::FS::IOFile file{build_file, Common::FS::FileAccessMode::Read};
        if (!file.IsOpen()) {
            return;
        }
        cached_build.resize(file.GetSize());
        file.ReadSpan(std::span{cached_build.data(), cached_build.size()});
    }
    const auto target = cached_build == build_hash
                            ? build_dir
                            : game_dir / (cached_build.size() >= 16 ? cached_build.substr(0, 16)
                                                                    : std::string{"old"});
    std::filesystem::create_directories(target, ec);
    // The files are listed first: moving them while the folder is being iterated is not safe, and
    // the iterator's own increment would throw on an error instead of reporting it.
    std::vector<std::filesystem::path> files;
    ec.clear();
    for (std::filesystem::directory_iterator it{game_dir, ec}, end; !ec && it != end;
         it.increment(ec)) {
        std::error_code entry_ec{};
        if (!it->is_regular_file(entry_ec)) {
            continue;
        }
        const auto ext = it->path().extension().string();
        if (ext == ".key" || ext == ".meta" || ext == ".spv" || ext == ".bin") {
            files.push_back(it->path());
        }
    }
    for (const auto& file : files) {
        std::filesystem::rename(file, target / file.filename(), ec);
    }
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();

    using namespace Common::FS;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_zero_struct(&zip_ar);

        cache_path = GetUserPath(PathType::CacheDir) /
                     std::filesystem::path{game_info.GameSerial()}.replace_extension(".zip");

        if (!mz_zip_reader_init_file(&zip_ar, cache_path.string().c_str(),
                                     MZ_ZIP_FLAG_READ_ALLOW_WRITING) ||
            !mz_zip_validate_archive(&zip_ar, 0)) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&zip_ar);
            mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0);
        }
    } else {
        // Every build that compiles shaders differently keeps its own cache next to the others
        // instead of wiping the previous one, so going back to a build finds its cache intact.
        const auto game_dir = GetUserPath(PathType::CacheDir) / game_info.GameSerial();
        const std::string build_hash{Common::g_shader_cache_hash};
        cache_path = game_dir / build_hash.substr(0, 16);
        std::error_code ec{};
        if (!std::filesystem::exists(cache_path, ec)) {
            std::filesystem::create_directories(cache_path, ec);
            MigrateFlatCache(game_dir, cache_path, build_hash);
        }
    }

    io_worker = std::jthread{ProcessIO};
    opened = true;
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    io_worker.request_stop();
    io_worker.join();

    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_finalize_archive(&zip_ar);
        mz_zip_writer_end(&zip_ar);
    }

    opened = false;
    LOG_INFO(Render, "Cache dumped");
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
    {
        auto request = std::packaged_task<void()>{[=]() {
            auto path{path_};
            path.replace_extension(GetBlobFileExtension(type));
            if (EmulatorSettings.IsPipelineCacheArchived()) {
                ASSERT_MSG(!ar_is_read_only,
                           "The archive is read-only. Did you forget to call `FinishPreload`?");
                if (!mz_zip_writer_add_mem(&zip_ar, path.string().c_str(), v.data(),
                                           v.size() * sizeof(T), MZ_BEST_COMPRESSION)) {
                    LOG_ERROR(Render, "Failed to add {} to the archive", path.string().c_str());
                }
            } else {
                using namespace Common::FS;
                const auto file = IOFile{path, FileAccessMode::Create};
                file.Write(v);
            }
        }};
        std::scoped_lock lock{m_request};
        req_queue.emplace(std::move(request));
    }
    request_cv.notify_one();
    return true;
}

template <typename T>
void LoadVector(BlobType type, std::filesystem::path& path, std::vector<T>& v) {
    using namespace Common::FS;
    path.replace_extension(GetBlobFileExtension(type));
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        int index{-1};
        index = mz_zip_reader_locate_file(&zip_ar, path.string().c_str(), nullptr, 0);
        if (index < 0) {
            LOG_WARNING(Render, "File {} is not found in the archive", path.string().c_str());
            return;
        }
        mz_zip_archive_file_stat stat{};
        mz_zip_reader_file_stat(&zip_ar, index, &stat);
        v.resize(stat.m_uncomp_size / sizeof(T));
        mz_zip_reader_extract_to_mem(&zip_ar, index, v.data(), stat.m_uncomp_size, 0);
    } else {
        const auto file = IOFile{path, FileAccessMode::Read};
        v.resize(file.GetSize() / sizeof(T));
        file.Read(v);
    }
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    const auto& ext = GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            file_name.fill(0);
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            if (std::string{file_name.data()}.ends_with(ext)) {
                mz_zip_archive_file_stat stat{};
                mz_zip_reader_file_stat(&zip_ar, index, &stat);
                std::vector<u8> data(stat.m_uncomp_size);
                mz_zip_reader_extract_to_mem(&zip_ar, index, data.data(), data.size(), 0);
                func(std::move(data));
            }
        }
    } else {
        // The files are listed first so a file that is removed, or an error while walking the
        // folder, cannot throw out of the iterator and take the emulator down while it warms up.
        std::vector<std::filesystem::path> files;
        std::error_code ec{};
        for (std::filesystem::directory_iterator it{cache_path, ec}, end; !ec && it != end;
             it.increment(ec)) {
            if (it->path().extension().string().ends_with(ext)) {
                files.push_back(it->path());
            }
        }
        for (const auto& path : files) {
            using namespace Common::FS;
            const auto& file = IOFile{path, FileAccessMode::Read};
            if (file.IsOpen()) {
                std::vector<u8> data(file.GetSize());
                if (file.Read(data) == data.size()) {
                    func(std::move(data));
                }
            }
        }
    }
}

void DataBase::ForEachBlobName(BlobType type,
                               const std::function<void(std::string_view name)>& func) {
    const auto ext = "." + GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            const std::string_view name{file_name.data()};
            if (name.ends_with(ext)) {
                func(name.substr(0, name.size() - ext.size()));
            }
        }
        return;
    }
    std::error_code ec{};
    for (std::filesystem::directory_iterator it{cache_path, ec}, end; !ec && it != end;
         it.increment(ec)) {
        if (it->path().extension().string() == ext) {
            func(it->path().stem().string());
        }
    }
}

bool DataBase::Reset() {
    if (!opened || EmulatorSettings.IsPipelineCacheArchived()) {
        return false;
    }
    std::error_code ec{};
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it{cache_path, ec}, end; !ec && it != end;
         it.increment(ec)) {
        const auto ext = it->path().extension().string();
        if (ext == ".key" || ext == ".meta" || ext == ".spv" || ext == ".bin") {
            files.push_back(it->path());
        }
    }
    for (const auto& file : files) {
        std::filesystem::remove(file, ec);
    }
    return true;
}

void DataBase::FinishPreload() {
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
    }
}

} // namespace Storage
