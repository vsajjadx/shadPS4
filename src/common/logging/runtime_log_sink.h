// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <memory>
#include <utility>
#include <vector>
#include <spdlog/sinks/async_sink.h>

namespace Common::Log {

// Diagnostic output must not stop guest CPU/GPU threads when the console, launcher or disk
// cannot keep up. Keep bounded memory and the most recent diagnostics under overload.
class RuntimeLogSink final : public spdlog::sinks::sink {
public:
    explicit RuntimeLogSink(std::vector<spdlog::sink_ptr> sinks, bool synchronous_critical,
                            size_t queue_size = spdlog::sinks::async_sink::default_queue_size)
        : sinks{std::move(sinks)}, synchronous_critical{synchronous_critical},
          async{std::make_shared<spdlog::sinks::async_sink>(
              spdlog::sinks::async_sink::config{
                  .queue_size = queue_size,
                  .policy = spdlog::sinks::async_sink::overflow_policy::overrun_oldest,
                  .sinks = this->sinks})} {}

    void log(const spdlog::details::log_msg& msg) override {
        if (!synchronous_critical || msg.log_level < spdlog::level::critical) {
            async->log(msg);
            return;
        }
        // Preserve the existing crash-report path. Only fatal diagnostics may wait on I/O.
        static_cast<void>(async->wait_all(std::chrono::milliseconds(250)));
        for (const auto& sink : sinks) {
            if (sink->should_log(msg.log_level)) {
                sink->log(msg);
                sink->flush();
            }
        }
    }

    void flush() override {
        // logger::flush_on() invokes this on the emitting guest thread too. Enqueue the flush;
        // draining the queue or directly flushing the backend here reintroduces I/O stalls on
        // every warning/error when the user enables a flush threshold.
        async->flush();
    }

    void set_pattern(const std::string&) override {}
    void set_formatter(std::unique_ptr<spdlog::formatter>) override {}

    [[nodiscard]] size_t OverrunCount() const {
        return async->get_overrun_counter();
    }

private:
    // Destroy the worker first so its queued messages drain while the backends still exist.
    std::vector<spdlog::sink_ptr> sinks;
    bool synchronous_critical;
    std::shared_ptr<spdlog::sinks::async_sink> async;
};

} // namespace Common::Log
