// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/types.h"
#include "core/libraries/libs.h"

void LinkSymbolImpl(Core::Loader::SymbolsResolver* sym, char const* nid, char const* lib,
                    u16 libversion, char const* mod, u64 symbol, Core::Loader::SymbolType sym_type,
                    char const* function_name) {}

namespace Core::Loader {

void SymbolsResolver::AddSymbol(const SymbolResolver& /*sym*/, u64 /*addr*/) {}

} // namespace Core::Loader

namespace Libraries::Kernel {

void RecordHleCall(const void* function, const void* caller) {}

} // namespace Libraries::Kernel
