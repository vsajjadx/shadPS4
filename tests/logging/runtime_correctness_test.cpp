// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "common/hack_features.h"
#include "common/logging/log.h"
#include "common/logging/verbose_log_budget.h"

// This CPU-only test links the real title classifier, without the emulator or GPU.
void assert_fail_impl() { std::abort(); }
namespace Common::Log {
std::array<Level, NUM_LOG_CLASSES> g_class_levels{};
static VerboseLogBudget macro_budget;
bool AdmitVerboseLog(Class) { return macro_budget.Allow(1); }
static unsigned emitted{};
void VLog(Class, Level, const char*, int, const char*, fmt::string_view, fmt::format_args) {
    ++emitted;
}
}

static void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int main(int argc, char** argv) {
    try {
        Require(argc > 1, "missing test case");
        const std::string mode{argv[1]};
        if (mode == "title") {
            Require(argc == 6, "missing classifier inputs");
            Common::HackFeatures::Init(argv[2], argv[3]);
            Require(Common::HackFeatures::isTheOrder1886 == (std::string{argv[4]} == "1"),
                    "The Order workaround leaked to another title or was disabled incorrectly");
            Require(Common::HackFeatures::isNhl24 == (std::string{argv[5]} == "1"),
                    "incorrect NHL classification");
        } else if (mode == "budget") {
            Common::Log::VerboseLogBudget budget;
            for (unsigned i = 0; i < budget.Limit; ++i) {
                Require(budget.Allow(10), "budget exhausted early");
            }
            Require(!budget.Allow(10), "unbounded diagnostic admitted");
            Require(budget.Allow(11), "new second did not replenish budget");
            for (unsigned i = 1; i < budget.Limit; ++i) {
                Require(budget.Allow(11), "new budget exhausted early");
            }
            Require(!budget.Allow(10), "delayed producer reset the clock backward");
            Require(budget.Suppressed() == 2, "incorrect suppression accounting");
        } else if (mode == "concurrent") {
            Common::Log::VerboseLogBudget budget;
            std::atomic<unsigned> admitted{};
            {
                std::vector<std::jthread> threads;
                for (unsigned i = 0; i < 16; ++i) {
                    threads.emplace_back([&] {
                        for (unsigned j = 0; j < 10000; ++j) {
                            if (budget.Allow(20)) {
                                ++admitted;
                            }
                        }
                    });
                }
            }
            Require(admitted == budget.Limit, "concurrent producers exceeded shared budget");
            Require(budget.Suppressed() == 160000 - budget.Limit,
                    "concurrent suppression count is inconsistent");
        } else if (mode == "macro") {
            using namespace Common::Log;
            g_class_levels.fill(Level::Trace);
            unsigned evaluated{};
            for (unsigned i = 0; i < 100000; ++i) {
                LOG_TRACE(Render, "hot path {}", ++evaluated);
            }
            Require(evaluated == VerboseLogBudget::Limit,
                    "suppressed trace arguments were still evaluated");
            Require(emitted == VerboseLogBudget::Limit, "trace output exceeded budget");
            LOG_INFO(Render, "info {}", ++evaluated);
            LOG_WARNING(Render, "warning {}", ++evaluated);
            LOG_ERROR(Render, "error {}", ++evaluated);
            LOG_CRITICAL(Render, "critical {}", ++evaluated);
            Require(emitted == VerboseLogBudget::Limit + 4,
                    "important diagnostics were incorrectly throttled");
            Require(evaluated == VerboseLogBudget::Limit + 4,
                    "important diagnostic arguments were suppressed");
        } else {
            throw std::runtime_error("unknown test case");
        }
        std::cout << "PASS: " << mode << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: " << ex.what() << '\n';
        return 1;
    }
}
