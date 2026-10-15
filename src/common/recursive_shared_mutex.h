// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <shared_mutex>

#include "common/assert.h"
#include "common/types.h"

namespace Common {

/// R2: a reader-writer lock a thread may take again while it holds it: exclusively inside an
/// exclusive hold, shared inside either. A thread holding it shared must not ask for it
/// exclusively (that would wait for itself); that is reported and fatal. One instance per Tag
/// (the per-thread depths are kept per Tag).
template <typename Tag>
class RecursiveSharedMutex {
public:
    void lock() {
        if (t_exclusive != 0) {
            ++t_exclusive;
            return;
        }
        ASSERT_MSG(t_shared == 0, "RecursiveSharedMutex: exclusive asked for inside a shared hold");
        mutex.lock();
        t_exclusive = 1;
    }

    void unlock() {
        if (--t_exclusive == 0) {
            mutex.unlock();
        }
    }

    void lock_shared() {
        if (t_exclusive != 0 || t_shared != 0) {
            ++t_shared;
            return;
        }
        mutex.lock_shared();
        t_shared = 1;
        t_shared_real = true;
    }

    void unlock_shared() {
        if (--t_shared == 0 && t_shared_real) {
            t_shared_real = false;
            mutex.unlock_shared();
        }
    }

    /// Whether this thread holds it exclusively.
    static bool HeldExclusive() noexcept {
        return t_exclusive != 0;
    }

private:
    std::shared_mutex mutex;
    static inline thread_local u32 t_exclusive{};
    static inline thread_local u32 t_shared{};
    static inline thread_local bool t_shared_real{};
};

} // namespace Common
