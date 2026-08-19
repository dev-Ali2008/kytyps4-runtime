// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/logging/log.h"
#include "core/libraries/libc_internal/libc_internal_cxa.h"
#include "core/libraries/libs.h"

namespace Libraries::LibcInternal {
namespace {

std::mutex GuardMutex;
std::condition_variable GuardCondition;
std::unordered_map<u64*, std::thread::id> GuardOwners;

struct AtexitEntry {
    void (*handler)(void*);
    void* arg;
    void* dso_handle;
};
std::vector<AtexitEntry> atexit_handlers;
std::mutex atexit_mutex;

u8* GuardBytes(u64* guard_object) {
    return reinterpret_cast<u8*>(guard_object);
}

bool IsInitialized(u64* guard_object) {
    return std::atomic_ref<u8>{GuardBytes(guard_object)[0]}.load(std::memory_order_acquire) != 0;
}

} // namespace

int PS4_SYSV_ABI fex_libc_cxa_guard_acquire(u64* guard_object) {
    std::unique_lock lock{GuardMutex};
    for (;;) {
        if (IsInitialized(guard_object)) {
            return 0;
        }

        const auto owner = GuardOwners.find(guard_object);
        if (owner == GuardOwners.end()) {
            GuardOwners.emplace(guard_object, std::this_thread::get_id());
            GuardBytes(guard_object)[1] = 1;
            return 1;
        }
        if (owner->second == std::this_thread::get_id()) {
            LOG_CRITICAL(Lib_LibcInternal,
                         "recursive initialization of guest static at {}",
                         static_cast<void*>(guard_object));
            std::terminate();
        }

        GuardCondition.wait(lock);
    }
}

void PS4_SYSV_ABI fex_libc_cxa_guard_release(u64* guard_object) {
    {
        std::lock_guard lock{GuardMutex};
        std::atomic_ref<u8>{GuardBytes(guard_object)[0]}.store(1, std::memory_order_release);
        GuardBytes(guard_object)[1] = 0;
        if (GuardOwners.erase(guard_object) != 1) {
            LOG_ERROR(Lib_LibcInternal, "release of unowned guest static guard at {}",
                      static_cast<void*>(guard_object));
        }
    }
    GuardCondition.notify_all();
}

void PS4_SYSV_ABI fex_libc_cxa_guard_abort(u64* guard_object) {
    {
        std::lock_guard lock{GuardMutex};
        GuardBytes(guard_object)[1] = 0;
        if (GuardOwners.erase(guard_object) != 1) {
            LOG_ERROR(Lib_LibcInternal, "abort of unowned guest static guard at {}",
                      static_cast<void*>(guard_object));
        }
    }
    GuardCondition.notify_all();
}

int PS4_SYSV_ABI fex_libc_cxa_atexit(void (*handler)(void*), void* arg, void* dso_handle) {
    if (!handler) {
        return -1;
    }
    std::lock_guard lock{atexit_mutex};
    atexit_handlers.push_back({handler, arg, dso_handle});
    LOG_DEBUG(Lib_LibcInternal, "registered atexit handler arg={} dso={}",
              static_cast<void*>(arg), static_cast<void*>(dso_handle));
    return 0;
}

int PS4_SYSV_ABI fex_libc_cxa_finalize(void* dso_handle) {
    std::lock_guard lock{atexit_mutex};
    auto it = atexit_handlers.begin();
    while (it != atexit_handlers.end()) {
        if (!dso_handle || it->dso_handle == dso_handle) {
            LOG_DEBUG(Lib_LibcInternal, "calling atexit handler arg={}",
                      static_cast<void*>(it->arg));
            it->handler(it->arg);
            it = atexit_handlers.erase(it);
        } else {
            ++it;
        }
    }
    return 0;
}

void RegisterFexLibcCxaAliases(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("3GPpjQdAMTw", "libc", 1, "libc", fex_libc_cxa_guard_acquire);
    LIB_FUNCTION("9rAeANT2tyE", "libc", 1, "libc", fex_libc_cxa_guard_release);
    LIB_FUNCTION("2emaaluWzUw", "libc", 1, "libc", fex_libc_cxa_guard_abort);
    LIB_FUNCTION("RCBypXp5h1U", "libc", 1, "libc", fex_libc_cxa_atexit);
    LIB_FUNCTION("p+pV1dCxu10", "libc", 1, "libc", fex_libc_cxa_finalize);
}

} // namespace Libraries::LibcInternal
