// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <spdlog/logger.h>
#include "common/logging/runtime_log_sink.h"

using namespace std::chrono_literals;

namespace {
void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Stall only the logging worker. Deterministic queue saturation, no reliance on disk speed.
class GatedSink final : public spdlog::sinks::sink {
public:
    void log(const spdlog::details::log_msg& msg) override {
        std::unique_lock lock(mutex);
        if (msg.log_level != spdlog::level::critical) {
            entered = true;
            cv.notify_all();
            cv.wait(lock, [this] { return released; });
        } else {
            critical_seen = true;
        }
        messages.emplace_back(msg.payload);
    }
    void flush() override { ++flushes; }
    void set_pattern(const std::string&) override {}
    void set_formatter(std::unique_ptr<spdlog::formatter>) override {}
    bool WaitUntilBlocked() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 5s, [this] { return entered; });
    }
    void Release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
    bool HasMessage(const std::string& expected) {
        std::lock_guard lock(mutex);
        for (const auto& message : messages) {
            if (message == expected) {
                return true;
            }
        }
        return false;
    }
    std::atomic<bool> critical_seen{false};
    std::atomic<unsigned> flushes{0};
private:
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{false};
    bool released{false};
    std::vector<std::string> messages;
};

void SaturatedQueueDoesNotBlock(bool sync_critical) {
    auto backend = std::make_shared<GatedSink>();
    auto sink = std::make_shared<Common::Log::RuntimeLogSink>(
        std::vector<spdlog::sink_ptr>{backend}, sync_critical, 8);
    spdlog::logger logger("runtime-test", sink);
    logger.info("block-worker");
    const bool worker_blocked = backend->WaitUntilBlocked();
    if (!worker_blocked) {
        backend->Release();
        Require(false, "worker did not start");
    }
    // Exercise logger::flush_on, not just sink::log. The former used to drain and perform
    // synchronous backend I/O for every error even when the queue was not full.
    logger.flush_on(spdlog::level::err);
    auto producer = std::async(std::launch::async, [&] {
        for (int i = 0; i < 20000; ++i) {
            logger.error("streaming diagnostic {}", i);
        }
        logger.info("last diagnostic");
    });
    const bool returned = producer.wait_for(2s) == std::future_status::ready;
    const auto overruns = sink->OverrunCount();
    const bool flushed_on_producer = backend->flushes.load() != 0;
    backend->Release(); // Always unblock before assertions/destructors, including on regression.
    producer.get();
    logger.flush();
    logger.sinks().clear();
    sink.reset(); // Destruction must drain the final queued diagnostic.
    Require(returned, "guest logging blocked behind stalled backend");
    Require(overruns > 0, "test failed to saturate the queue");
    Require(!flushed_on_producer, "flush_on performed backend I/O on the producer");
    Require(backend->HasMessage("last diagnostic"), "newest diagnostic lost");
    std::cout << "PASS: saturated queue + flush_on, sync=" << sync_critical
              << ", overruns=" << overruns << '\n';
}

void CriticalBypassesSaturatedQueue() {
    auto backend = std::make_shared<GatedSink>();
    auto sink = std::make_shared<Common::Log::RuntimeLogSink>(
        std::vector<spdlog::sink_ptr>{backend}, true, 8);
    spdlog::logger logger("crash-test", sink);
    logger.info("block-worker");
    const bool started = backend->WaitUntilBlocked();
    if (!started) {
        backend->Release();
        Require(false, "worker did not start");
    }
    for (int i = 0; i < 64; ++i) {
        logger.info("queued {}", i);
    }
    logger.critical("fatal checkpoint");
    const bool delivered = backend->critical_seen.load();
    const bool flushed = backend->flushes.load() > 0;
    backend->Release();
    Require(delivered, "critical report not delivered synchronously");
    Require(flushed, "critical report not flushed synchronously");
    std::cout << "PASS: critical crash report survives queue saturation\n";
}

void MultiProducerStress() {
    auto backend = std::make_shared<GatedSink>();
    backend->Release();
    auto sink = std::make_shared<Common::Log::RuntimeLogSink>(
        std::vector<spdlog::sink_ptr>{backend}, true, 32);
    spdlog::logger logger("multi-producer-test", sink);
    {
        std::vector<std::jthread> producers;
        for (unsigned t = 0; t < 8; ++t) {
            producers.emplace_back([&, t] {
                for (unsigned i = 0; i < 10000; ++i) {
                    logger.warn("thread {} diagnostic {}", t, i);
                }
            });
        }
    }
    logger.info("all producers finished");
    logger.sinks().clear();
    sink.reset();
    Require(backend->HasMessage("all producers finished"), "shutdown did not drain queue");
    std::cout << "PASS: eight producers, 80000 messages, shutdown drain\n";
}
} // namespace

int main() {
    try {
        SaturatedQueueDoesNotBlock(false);
        SaturatedQueueDoesNotBlock(true);
        CriticalBypassesSaturatedQueue();
        MultiProducerStress();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: " << ex.what() << '\n';
        return 1;
    }
}
