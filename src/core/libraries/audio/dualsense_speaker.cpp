// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Ported from KytyPS5 (https://github.com/KytyPS5/KytyPS5, MIT): src/libs/dualSenseHaptics.cpp
// and src/libs/dualSenseBluetooth.cpp, reduced to the pad speaker.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_hidapi.h>
#include <opus.h>

#include "common/logging/log.h"
#include "core/libraries/audio/dualsense_speaker.h"

namespace Libraries::AudioOut::DualSenseSpeaker {

namespace Bluetooth {

struct Stream;

namespace {
// Report 0x39 and its sub-packets are described by awalol/DS5Dongle (MIT):
// https://github.com/awalol/DS5Dongle
// Each report plays two 32-frame stereo haptic blocks at 3 kHz (21.333 ms).
constexpr uint64_t PERIOD_NS   = 64000000000ULL / 3000;
constexpr int      HAPTIC_RATE = 3000;
constexpr int      HAPTIC_REPORT_FLOATS = 2 * 32 * 2;
// Two 480-frame Opus packets occupy the same 21.333 ms slot. Resampling
// 48 kHz input to 45 kHz before encoding at 48 kHz compensates for playback.
constexpr int    SPEAKER_RATE = 45000;
constexpr int    OPUS_FRAMES  = 480;
constexpr int    OPUS_BYTES   = 200;
constexpr size_t REPORT_BYTES = 547;
constexpr int    MAX_QUEUE_MS = 80;
constexpr uint64_t COALESCE_NS = 1000000;

struct Session;
std::mutex                              g_mutex;
std::condition_variable                 g_wake;
std::map<int, std::shared_ptr<Session>> g_sessions;
SDL_Thread*                             g_thread   = nullptr;
bool                                    g_stopping = false;
void (*g_tick)()                                   = nullptr;

template <typename Char>
std::string Address(const Char* serial) {
    std::string address;
    if (serial == nullptr) {
        return address;
    }
    for (; *serial != 0; ++serial) {
        if (*serial >= '0' && *serial <= '9') {
            address.push_back(static_cast<char>(*serial));
        } else if (*serial >= 'a' && *serial <= 'f') {
            address.push_back(static_cast<char>(*serial));
        } else if (*serial >= 'A' && *serial <= 'F') {
            address.push_back(static_cast<char>(*serial - 'A' + 'a'));
        } else if (*serial != ':' && *serial != '-') {
            return {};
        }
    }
    return address.size() == 12 ? address : std::string {};
}

SDL_hid_device* OpenDevice(int controller) {
    SDL_LockJoysticks();
    auto*      pad      = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(controller));
    const bool wireless = pad != nullptr && SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_PS5 &&
                          SDL_GetGamepadConnectionState(pad) == SDL_JOYSTICK_CONNECTION_WIRELESS;
    const auto vendor  = wireless ? SDL_GetGamepadVendor(pad) : 0;
    const auto product = wireless ? SDL_GetGamepadProduct(pad) : 0;
    const char* serial = wireless ? SDL_GetGamepadSerial(pad) : nullptr;
    const auto address = Address(serial);
    SDL_UnlockJoysticks();
    if (vendor != 0x054c || (product != 0x0ce6 && product != 0x0df2)) {
        LOG_WARNING(Lib_AudioOut, "DualSense Bluetooth: not a wireless DualSense ({:04x}:{:04x})",
                    vendor, product);
        return nullptr;
    }
    // Match the HID interface by the controller's Bluetooth address. Windows doesn't always
    // report it the same way on both sides (or SDL has no serial for the pad), so with exactly
    // one candidate interface that one is used.
    auto*       devices = SDL_hid_enumerate(vendor, product);
    std::string matched, only;
    int         candidates = 0;
    bool        ambiguous  = false;
    for (auto* device = devices; device != nullptr; device = device->next) {
        if (device->path == nullptr || device->bus_type == SDL_HID_API_BUS_USB ||
            !(device->usage_page == 0 || (device->usage_page == 1 && device->usage == 5))) {
            continue;
        }
        ++candidates;
        only = device->path;
        if (!address.empty() && Address(device->serial_number) == address) {
            if (!matched.empty()) {
                ambiguous = true;
            }
            matched = device->path;
        }
    }
    SDL_hid_free_enumeration(devices);
    const std::string path = !ambiguous && !matched.empty() ? matched
                             : candidates == 1             ? only
                                                           : std::string{};
    if (path.empty()) {
        LOG_WARNING(Lib_AudioOut,
                    "DualSense Bluetooth: no HID interface for the controller (serial '{}', "
                    "{} candidates)",
                    serial != nullptr ? serial : "", candidates);
        return nullptr;
    }
    auto* hid = SDL_hid_open_path(path.c_str());
    if (hid == nullptr) {
        LOG_WARNING(Lib_AudioOut, "DualSense Bluetooth: cannot open the HID interface: {}",
                    SDL_GetError());
    } else {
        LOG_INFO(Lib_AudioOut, "DualSense Bluetooth: opened the HID interface for speaker audio");
    }
    return hid;
}

void FinishReport(std::array<uint8_t, REPORT_BYTES>& report) {
    const uint8_t prefix = 0xa2;
    auto          crc    = SDL_crc32(0, &prefix, 1);
    crc                  = SDL_crc32(crc, report.data(), report.size() - 4);
    for (size_t i = 0; i < 4; i++) {
        report[report.size() - 4 + i] = static_cast<uint8_t>(crc >> (8 * i));
    }
}

struct Session {
    SDL_hid_device*       hid     = nullptr;
    OpusEncoder*          encoder = nullptr;
    std::vector<Stream*>  streams;
    std::mutex            io_mutex;
    std::atomic<bool>     active {true};
    std::atomic<bool>     failed {false};
    std::atomic<uint64_t> generation {0};
    uint64_t              next_send        = 0;
    uint64_t              pending_deadline = 0;
    uint8_t               sequence  = 0;
    uint8_t               counter   = 0;

    ~Session() {
        if (encoder != nullptr) {
            opus_encoder_destroy(encoder);
        }
        if (hid != nullptr) {
            SDL_hid_close(hid);
        }
    }

    bool CreateEncoder() {
        if (encoder != nullptr) {
            return true;
        }
        int error = OPUS_OK;
        encoder   = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
        if (encoder == nullptr || error != OPUS_OK) {
            return false;
        }
        if (opus_encoder_ctl(encoder, OPUS_SET_BITRATE(OPUS_BYTES * 8 * 100)) == OPUS_OK &&
            opus_encoder_ctl(encoder, OPUS_SET_VBR(0)) == OPUS_OK &&
            opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0)) == OPUS_OK) {
            return true;
        }
        opus_encoder_destroy(encoder);
        encoder = nullptr;
        return false;
    }
};
} // namespace

struct Stream {
    std::shared_ptr<Session> session;
    SDL_AudioStream*         audio   = nullptr;
    uint32_t                 freq    = 0;
    bool                     speaker = false;
};

namespace {
template <size_t N>
void Mix(SDL_AudioStream* stream, std::array<float, N>& mixed) {
    std::array<float, N> pcm {};
    const int            bytes = SDL_GetAudioStreamData(stream, pcm.data(), sizeof(pcm));
    for (int i = 0; i < std::max(0, bytes) / static_cast<int>(sizeof(float)); i++) {
        mixed[i] += pcm[i];
    }
}

struct PendingReport {
    std::shared_ptr<Session>          session;
    uint64_t                          generation;
    std::array<uint8_t, REPORT_BYTES> data {};
    std::array<float, 2 * OPUS_FRAMES * 2> audio {};
    bool                              speaker = false;
};

bool PrepareReport(Session& session, PendingReport& pending, uint64_t now) {
    bool has_data      = false;
    bool ready         = false;
    bool speaker_ready = false;
    for (const auto* stream: session.streams) {
        const bool available = SDL_GetAudioStreamAvailable(stream->audio) > 0;
        // Both payloads cover 64 / 3000 seconds. Measure input duration so SDL's
        // resampler lookahead does not make a complete block appear partial.
        const auto frames =
            (static_cast<uint64_t>(stream->freq) * (HAPTIC_REPORT_FLOATS / 2) + HAPTIC_RATE - 1) /
            HAPTIC_RATE;
        const bool full = available && SDL_GetAudioStreamQueued(stream->audio) >=
                                           static_cast<int>(frames * 2 * sizeof(float));
        has_data |= available;
        ready |= full;
        speaker_ready |= stream->speaker && full;
        pending.speaker |= stream->speaker;
    }
    if (!has_data) {
        session.pending_deadline = 0;
        return false;
    }
    if (!ready || (!speaker_ready && (pending.speaker || session.next_send == 0))) {
        // Accumulate short blocks for at most one report period. A full stream
        // must not wait on a partial peer; only briefly coalesce haptics with
        // speaker audio arriving in the same emulated batch.
        const auto deadline = now + (ready ? COALESCE_NS : PERIOD_NS);
        session.pending_deadline =
            session.pending_deadline == 0 ? deadline : std::min(session.pending_deadline, deadline);
        if (now < session.pending_deadline) {
            return false;
        }
    }
    session.pending_deadline = 0;
    std::array<float, HAPTIC_REPORT_FLOATS> haptics {};
    for (auto* stream: session.streams) {
        if (stream->speaker) {
            Mix(stream->audio, pending.audio);
        } else {
            Mix(stream->audio, haptics);
        }
    }
    auto& report     = pending.data;
    report[0]        = 0x39;
    report[1]        = static_cast<uint8_t>(session.sequence << 4);
    session.sequence = (session.sequence + 1) & 0x0f;
    report[2]        = 0x91;
    report[3]        = 6;
    report[4]        = 0x7e;
    std::fill_n(report.begin() + 5, 4, 48);
    session.counter += 2;
    report[9]  = session.counter;
    report[10] = 0xd2;
    report[11] = 64;
    for (size_t i = 0; i < haptics.size(); i++) {
        const auto value = std::isfinite(haptics[i]) ? haptics[i] : 0.0f;
        report[12 + i]   = static_cast<uint8_t>(
            static_cast<int>(std::lround(std::clamp(value, -1.0f, 1.0f) * 127)));
    }
    if (pending.speaker) {
        report[140] = 0xd3;
        report[141] = OPUS_BYTES;
    }
    return true;
}

bool EncodeReport(PendingReport& pending) {
    if (!pending.session->active || pending.session->failed) {
        return false;
    }
    if (pending.speaker) {
        for (auto& sample: pending.audio) {
            sample = std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0.0f;
        }
        for (int packet = 0; packet < 2; packet++) {
            if (opus_encode_float(pending.session->encoder,
                                  pending.audio.data() + packet * OPUS_FRAMES * 2, OPUS_FRAMES,
                                  pending.data.data() + 142 + packet * OPUS_BYTES,
                                  OPUS_BYTES) != OPUS_BYTES) {
                pending.session->failed = true;
                return false;
            }
        }
    }
    FinishReport(pending.data);
    return true;
}

std::vector<PendingReport> Prepare(uint64_t now) {
    std::vector<PendingReport> pending;
    {
        std::lock_guard lock(g_mutex);
        for (auto& [id, session]: g_sessions) {
            if (session->failed || now < session->next_send) {
                continue;
            }
            PendingReport report {session, session->generation.load()};
            if (PrepareReport(*session, report, now)) {
                pending.push_back(std::move(report));
                // Regain timing after a late HID write without sending reports in a
                // burst. Reset the cadence only on the first report.
                session->next_send = session->next_send == 0
                                         ? now + PERIOD_NS
                                         : std::max(session->next_send + PERIOD_NS,
                                                    now + PERIOD_NS / 2);
            }
        }
    }
    // Opus encoding can take milliseconds; the emulation audio thread must be able
    // to queue its next block while this work runs.
    pending.erase(std::remove_if(pending.begin(), pending.end(),
                                 [](PendingReport& report) { return !EncodeReport(report); }),
                  pending.end());
    return pending;
}

void Send(const std::vector<PendingReport>& pending) {
    for (const auto& report: pending) {
        auto&           session = *report.session;
        std::lock_guard io_lock(session.io_mutex);
        if (!session.active || session.failed || report.generation != session.generation) {
            continue;
        }
        if (SDL_hid_write(session.hid, report.data.data(), report.data.size()) !=
            static_cast<int>(report.data.size())) {
            session.failed = true;
            LOG_WARNING(Lib_AudioOut, "DualSense Bluetooth: audio HID write failed: {}", SDL_GetError());
        }
    }
}

int SDLCALL Sender(void*) {
    std::unique_lock lock(g_mutex);
    while (!g_stopping) {
        auto tick = g_tick;
        lock.unlock();
        auto pending = Prepare(SDL_GetTicksNS());
        Send(pending);
        if (tick != nullptr) {
            tick();
        }
        lock.lock();
        if (g_sessions.empty()) {
            g_wake.wait(lock, [] { return g_stopping || !g_sessions.empty(); });
        } else {
            const auto now = SDL_GetTicksNS();
            uint64_t   wait_ns = 10000000; // Check rumble expiry at most 10 ms late.
            for (const auto& [id, session]: g_sessions) {
                if (session->next_send > now) {
                    wait_ns = std::min(wait_ns, session->next_send - now);
                }
                if (session->pending_deadline > now) {
                    wait_ns = std::min(wait_ns, session->pending_deadline - now);
                }
            }
            g_wake.wait_for(lock, std::chrono::nanoseconds(wait_ns));
        }
    }
    return 0;
}
} // namespace

Stream* Open(uint32_t freq, bool speaker, int controller, void (*tick)()) {
    if (freq == 0 || freq > 192000) {
        return nullptr;
    }
    std::lock_guard lock(g_mutex);
    if (g_stopping) {
        return nullptr;
    }
    auto                     it = g_sessions.find(controller);
    std::shared_ptr<Session> session;
    if (it != g_sessions.end() && !it->second->failed) {
        session = it->second;
    } else {
        session      = std::make_shared<Session>();
        session->hid = OpenDevice(controller);
        if (session->hid == nullptr) {
            return nullptr;
        }
    }
    if (speaker && !session->CreateEncoder()) {
        LOG_WARNING(Lib_AudioOut, "DualSense Bluetooth: cannot create the Opus encoder");
        return nullptr;
    }
    const SDL_AudioSpec source {SDL_AUDIO_F32, 2, static_cast<int>(freq)};
    const SDL_AudioSpec target {SDL_AUDIO_F32, 2, speaker ? SPEAKER_RATE : HAPTIC_RATE};
    auto*               audio = SDL_CreateAudioStream(&source, &target);
    if (audio == nullptr) {
        return nullptr;
    }
    if (g_thread == nullptr) {
        g_thread = SDL_CreateThread(Sender, "DualSense Bluetooth audio", nullptr);
        if (g_thread == nullptr) {
            SDL_DestroyAudioStream(audio);
            return nullptr;
        }
    }
    g_tick       = tick;
    auto* stream = new Stream {session, audio, freq, speaker};
    session->streams.push_back(stream);
    session->pending_deadline = 0;
    g_sessions[controller] = session;
    g_wake.notify_one();
    return stream;
}

uint64_t Queue(Stream* stream, const float* stereo, uint32_t frames) {
    if (stream == nullptr || stereo == nullptr || frames == 0) {
        return 0;
    }
    std::lock_guard lock(g_mutex);
    if (!stream->session->active || stream->session->failed) {
        return 0;
    }
    const uint32_t max_frames = stream->freq * MAX_QUEUE_MS / 1000;
    if (frames > max_frames) {
        stereo += static_cast<size_t>(frames - max_frames) * 2;
        frames = max_frames;
    }
    int       queued = std::max(0, SDL_GetAudioStreamQueued(stream->audio));
    const int bytes  = static_cast<int>(frames * 2 * sizeof(float));
    if (queued + bytes > static_cast<int>(max_frames * 2 * sizeof(float))) {
        // Discard complete old report slots, preserving the newest queued audio.
        // Clearing the entire stream here creates an audible gap after a brief
        // Windows Bluetooth write stall.
        std::array<float, 2 * OPUS_FRAMES * 2> discarded {};
        const int discard_bytes = stream->speaker
                                      ? static_cast<int>(sizeof(discarded))
                                      : static_cast<int>(HAPTIC_REPORT_FLOATS * sizeof(float));
        const int target = static_cast<int>(max_frames * 2 * sizeof(float)) - bytes;
        while (queued > target) {
            if (SDL_GetAudioStreamData(stream->audio, discarded.data(), discard_bytes) <= 0) {
                SDL_ClearAudioStream(stream->audio);
                break;
            }
            queued = std::max(0, SDL_GetAudioStreamQueued(stream->audio));
        }
    }
    queued = std::max(0, SDL_GetAudioStreamQueued(stream->audio));
    if (!SDL_PutAudioStreamData(stream->audio, stereo, bytes)) {
        return 0;
    }
    g_wake.notify_one();
    return static_cast<uint64_t>(queued + bytes) * 1000000 / (stream->freq * 2 * sizeof(float));
}

void Close(Stream* stream) {
    if (stream == nullptr) {
        return;
    }
    auto session = stream->session;
    {
        std::lock_guard lock(g_mutex);
        auto&           streams = session->streams;
        streams.erase(std::remove(streams.begin(), streams.end(), stream), streams.end());
        session->generation++;
        session->pending_deadline = 0;
        if (streams.empty()) {
            session->active = false;
            for (auto it = g_sessions.begin(); it != g_sessions.end(); ++it) {
                if (it->second == session) {
                    g_sessions.erase(it);
                    break;
                }
            }
        }
        SDL_DestroyAudioStream(stream->audio);
        delete stream;
    }
    // Drain a write already in flight before the caller changes speaker routing.
    std::lock_guard io_lock(session->io_mutex);
}

void Shutdown() {
    SDL_Thread* thread;
    {
        std::lock_guard lock(g_mutex);
        g_stopping = true;
        for (auto& [id, session]: g_sessions) {
            session->active = false;
        }
        thread = g_thread;
        g_wake.notify_one();
    }
    if (thread != nullptr) {
        SDL_WaitThread(thread, nullptr);
    }
    std::lock_guard lock(g_mutex);
    g_sessions.clear();
    g_thread   = nullptr;
    g_tick     = nullptr;
    g_stopping = false;
}


} // namespace Bluetooth

struct Stream {
    u32 freq = 0;
    SDL_JoystickID controller = 0;
    SDL_AudioStream* sdl = nullptr;
    Bluetooth::Stream* bluetooth = nullptr;
    SDL_AudioDeviceID device = 0;
    u64 next_check = 0;
    std::vector<float> buffer;
};

namespace {

constexpr int FRAME_BYTES = 4 * sizeof(float);
constexpr u32 MAX_QUEUE_MS = 80;
std::mutex g_mutex;
SDL_JoystickID g_speaker_controller = 0;
std::atomic<int> g_last_status{-1};

/// Logs why the speaker audio isn't (or is again) going to a DualSense, once per change.
void ReportStatus(int status, const char* message) {
    if (g_last_status.exchange(status) != status) {
        LOG_INFO(Lib_AudioOut, "DualSense speaker: {}", message);
    }
}
std::map<SDL_JoystickID, int> g_speaker_streams;

bool RouteSpeaker(SDL_JoystickID controller, bool speaker) { // Caller holds g_mutex.
    // Route the right channel to the speaker and mute the headphones with the values Linux uses
    // without headphones (output report 0x02). Plugged-in headphones are not checked.
    std::array<u8, 38> report{};
    report[0] = 0x80; // Audio control valid
    if (speaker) {
        report[0] |= 0x20; // Speaker volume valid
        report[1] = 0x80;  // Audio control 2 valid
        report[5] = 0x64;  // Speaker volume
        report[7] = 0x30;  // Output path: right channel to the speaker, headphones muted
        report[37] = 0x02; // Speaker preamp +6 dB
    }
    bool routed = false;
    SDL_LockJoysticks();
    if (auto* pad = SDL_GetGamepadFromID(controller); pad != nullptr) {
        routed = SDL_SendGamepadEffect(pad, report.data(), static_cast<int>(report.size()));
    }
    SDL_UnlockJoysticks();
    return routed;
}

bool SelectSpeaker(SDL_JoystickID controller) { // Caller holds g_mutex; 0 restores headphones.
    if (g_speaker_controller == controller) {
        return true;
    }
    if (g_speaker_controller != 0) {
        RouteSpeaker(g_speaker_controller, false);
    }
    g_speaker_controller = 0;
    if (controller != 0 && !RouteSpeaker(controller, true)) {
        return false;
    }
    g_speaker_controller = controller;
    return true;
}

void CloseDevice(Stream* stream) {
    const bool had_device = stream->sdl != nullptr || stream->bluetooth != nullptr;
    if (stream->bluetooth != nullptr) {
        Bluetooth::Close(stream->bluetooth);
        stream->bluetooth = nullptr;
    }
    if (stream->sdl != nullptr) {
        SDL_DestroyAudioStream(stream->sdl);
        stream->sdl = nullptr;
    }
    if (had_device) {
        std::scoped_lock lock{g_mutex};
        // Route back to the headphones once no speaker port plays on the controller.
        if (const auto it = g_speaker_streams.find(stream->controller);
            it != g_speaker_streams.end() && --it->second == 0) {
            g_speaker_streams.erase(it);
            if (g_speaker_controller == stream->controller) {
                SelectSpeaker(0);
            }
        }
    }
}

/// The DualSense the speaker audio goes to: the only wired one, else the first wireless one.
/// SDL exposes no association between a gamepad and its USB audio endpoint, so with several
/// wired DualSense controllers none is guessed.
SDL_JoystickID FindController(bool& wireless) {
    SDL_LockJoysticks();
    int count = 0;
    auto* pads = SDL_GetGamepads(&count);
    SDL_JoystickID wired_id = 0, wireless_id = 0;
    int wired_count = 0;
    for (int i = 0; pads != nullptr && i < count; i++) {
        auto* pad = SDL_GetGamepadFromID(pads[i]);
        if (pad == nullptr || SDL_GetGamepadTypeForID(pads[i]) != SDL_GAMEPAD_TYPE_PS5) {
            continue;
        }
        const auto state = SDL_GetGamepadConnectionState(pad);
        if (state == SDL_JOYSTICK_CONNECTION_WIRED) {
            wired_count++;
            wired_id = pads[i];
        } else if (state == SDL_JOYSTICK_CONNECTION_WIRELESS && wireless_id == 0) {
            wireless_id = pads[i];
        }
    }
    SDL_free(pads);
    SDL_UnlockJoysticks();
    if (wired_count == 1) {
        wireless = false;
        return wired_id;
    }
    wireless = true;
    return wired_count == 0 ? wireless_id : 0;
}

SDL_AudioDeviceID FindDevice() {
    int count = 0;
    auto* devices = SDL_GetAudioPlaybackDevices(&count);
    SDL_AudioDeviceID device = 0;
    for (int i = 0; devices != nullptr && i < count; i++) {
        const char* name = SDL_GetAudioDeviceName(devices[i]);
        SDL_AudioSpec spec{};
        const bool is_dualsense_name =
            name != nullptr && (SDL_strcasestr(name, "DualSense") != nullptr ||
                                SDL_strcasestr(name, "Wireless Controller") != nullptr);
        if (is_dualsense_name && SDL_GetAudioDeviceFormat(devices[i], &spec, nullptr) &&
            spec.channels == 4) {
            if (device != 0) {
                device = 0; // Multiple matching endpoints cannot be associated reliably either.
                break;
            }
            device = devices[i];
        }
    }
    SDL_free(devices);
    return device;
}

void RefreshDevice(Stream* stream, SDL_JoystickID controller, bool wireless) {
    if (stream->controller != controller || (wireless && stream->sdl != nullptr) ||
        (!wireless && stream->bluetooth != nullptr)) {
        CloseDevice(stream);
        stream->controller = controller;
        stream->next_check = 0;
    }
    const auto now = SDL_GetTicks();
    if (stream->bluetooth != nullptr || now < stream->next_check) {
        return;
    }
    stream->next_check = now + 2000;
    const auto device = wireless ? 0 : FindDevice();
    if (stream->device == device && stream->sdl != nullptr) {
        return;
    }
    CloseDevice(stream);
    stream->device = device;
    if (!wireless && device == 0) {
        return;
    }
    SDL_AudioStream* sdl = nullptr;
    Bluetooth::Stream* bluetooth = nullptr;
    bool ready;
    if (wireless) {
        bluetooth = Bluetooth::Open(stream->freq, true, static_cast<int>(controller), nullptr);
        ready = bluetooth != nullptr;
    } else {
        const SDL_AudioSpec desired{SDL_AUDIO_F32, 4, static_cast<int>(stream->freq)};
        SDL_AudioSpec actual{};
        sdl = SDL_OpenAudioDeviceStream(device, &desired, nullptr, nullptr);
        ready = sdl != nullptr &&
                SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(sdl), &actual, nullptr) &&
                actual.channels == 4 && SDL_ResumeAudioStreamDevice(sdl);
    }
    if (ready) {
        std::scoped_lock lock{g_mutex};
        ready = SelectSpeaker(controller);
        if (ready) {
            g_speaker_streams[controller]++;
        }
    }
    if (ready) {
        stream->sdl = sdl;
        stream->bluetooth = bluetooth;
        LOG_INFO(Lib_AudioOut, "DualSense speaker: playing pad speaker audio over {}",
                 wireless ? "Bluetooth" : SDL_GetAudioDeviceName(device));
    } else {
        Bluetooth::Close(bluetooth);
        if (sdl != nullptr) {
            SDL_DestroyAudioStream(sdl);
        }
        LOG_WARNING(Lib_AudioOut, "DualSense speaker: cannot open the controller audio: {}",
                    SDL_GetError());
    }
}

u64 QueueUsbAudio(Stream* stream, u32 frames) {
    auto queued = std::max(0, SDL_GetAudioStreamQueued(stream->sdl));
    const int bytes = static_cast<int>(frames * FRAME_BYTES);
    const int max_bytes =
        static_cast<int>(static_cast<u64>(stream->freq) * FRAME_BYTES * MAX_QUEUE_MS / 1000);
    if (queued + bytes > max_bytes) {
        // Keep the newest sound after a brief device stall. Clearing the entire stream produces
        // a gap that is much longer than one audio block.
        SDL_AudioSpec target{};
        std::array<u8, 16384> discarded{};
        if (SDL_GetAudioStreamFormat(stream->sdl, nullptr, &target)) {
            const int frame_bytes = SDL_AUDIO_FRAMESIZE(target);
            if (frame_bytes > 0) {
                const int chunk_frames = std::min<int>(
                    static_cast<int>(discarded.size()) / frame_bytes,
                    static_cast<int>((static_cast<u64>(frames) * target.freq + stream->freq - 1) /
                                     stream->freq));
                while (queued + bytes > max_bytes && chunk_frames > 0 &&
                       SDL_GetAudioStreamData(stream->sdl, discarded.data(),
                                              chunk_frames * frame_bytes) > 0) {
                    queued = std::max(0, SDL_GetAudioStreamQueued(stream->sdl));
                }
            }
        }
        if (queued + bytes > max_bytes) {
            SDL_ClearAudioStream(stream->sdl);
            queued = 0;
        }
    }
    if (!SDL_PutAudioStreamData(stream->sdl, stream->buffer.data(), bytes)) {
        return 0;
    }
    return static_cast<u64>(queued + bytes) * 1000000 / (stream->freq * FRAME_BYTES);
}

} // namespace

Stream* Open(u32 freq) {
    if (freq == 0 || !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        return nullptr;
    }
    auto* stream = new Stream{};
    stream->freq = freq;
    return stream;
}

void Close(Stream* stream) {
    if (stream != nullptr) {
        CloseDevice(stream);
        delete stream;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

u64 Queue(Stream* stream, const float* data, u32 frames, u32 channels, float gain) {
    if (stream == nullptr || data == nullptr || frames == 0 || channels == 0) {
        return 0;
    }
    const auto max_frames = static_cast<u32>(static_cast<u64>(stream->freq) * MAX_QUEUE_MS / 1000);
    const auto first_frame = frames > max_frames ? frames - max_frames : 0;
    frames -= first_frame;
    if (frames == 0) {
        return 0;
    }
    bool wireless = false;
    const SDL_JoystickID controller = FindController(wireless);
    if (controller == 0) {
        ReportStatus(1, "no DualSense connected (or several wired), pad speaker audio plays "
                        "on the pad speaker output device");
        CloseDevice(stream);
        stream->next_check = 0; // Look for the device as soon as a DualSense is active again.
        return 0;
    }
    RefreshDevice(stream, controller, wireless);
    if (stream->sdl == nullptr && stream->bluetooth == nullptr) {
        ReportStatus(2, wireless ? "the DualSense's Bluetooth audio could not be opened"
                                 : "no DualSense USB audio device (4 channels) found");
        return 0;
    }
    {
        std::scoped_lock lock{g_mutex};
        if (!SelectSpeaker(controller)) {
            ReportStatus(3, "the controller refused the speaker routing report");
            CloseDevice(stream);
            return 0;
        }
    }
    // USB places the speaker in front and the actuators in back; Bluetooth takes stereo.
    const u32 output_channels = wireless ? 2 : 4;
    stream->buffer.assign(static_cast<size_t>(frames) * output_channels, 0.0f);
    for (u32 frame = 0; frame < frames; frame++) {
        for (u32 ch = 0; ch < 2; ch++) {
            const u32 src_ch = channels == 1 ? 0 : ch;
            const size_t index = (static_cast<size_t>(first_frame) + frame) * channels + src_ch;
            stream->buffer[static_cast<size_t>(frame) * output_channels + ch] = data[index] * gain;
        }
    }
    const auto queued_us =
        wireless ? Bluetooth::Queue(stream->bluetooth, stream->buffer.data(), frames)
                 : QueueUsbAudio(stream, frames);
    if (queued_us == 0) {
        ReportStatus(4, "sending audio to the DualSense failed");
        CloseDevice(stream);
        stream->next_check = SDL_GetTicks() + 2000;
        return 0;
    }
    ReportStatus(0, wireless ? "playing on the DualSense speaker over Bluetooth"
                             : "playing on the DualSense speaker over USB");
    return queued_us;
}

void Shutdown() {
    Bluetooth::Shutdown();
    std::scoped_lock lock{g_mutex};
    SelectSpeaker(0);
}

} // namespace Libraries::AudioOut::DualSenseSpeaker
