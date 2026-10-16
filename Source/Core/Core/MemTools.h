// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace EMM
{
void InstallExceptionHandler();
void UninstallExceptionHandler();
bool IsExceptionHandlerSupported();

// Rev3 rollback, host write tracking (Core/Slippi/Rev3WriteTrack.h): a function that sees every memory-access fault BEFORE the JIT's
// fastmem backpatching does. It returns true when the fault was a write to a page it had protected itself and is dealt with (the faulting
// instruction is simply run again), false for every fault that is not its own, which then takes the usual path. It runs inside the signal /
// exception handler on whichever thread faulted, so it must be async-signal-safe. nullptr removes it. Returns false (and keeps nothing)
// where this platform's handler cannot call it (macOS Mach exceptions, platforms without a handler).
using WriteFaultHook = bool (*)(uintptr_t host_address);
bool SetWriteFaultHook(WriteFaultHook hook);
}  // namespace EMM
