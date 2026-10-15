// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include <source_location>

#include <atomic>
#include "common/perf_monitor.h"

#include <array>
#include <utility>

#include "common/types.h"

namespace VideoCore {

constexpr u64 PAGES_PER_WORD = 64;
constexpr u64 BYTES_PER_PAGE = 4_KB;
constexpr u64 BYTES_PER_WORD = PAGES_PER_WORD * BYTES_PER_PAGE;

constexpr u64 HIGHER_PAGE_BITS = 24;
constexpr u64 HIGHER_PAGE_SIZE = 1ULL << HIGHER_PAGE_BITS;
constexpr u64 HIGHER_PAGE_MASK = HIGHER_PAGE_SIZE - 1ULL;

constexpr u64 NUM_REGION_PAGES = HIGHER_PAGE_SIZE / BYTES_PER_PAGE;

/// PERF-012: upload epoch. Pages the CPU rewrites all the time ("hot") stay unprotected and are
/// uploaded at most once per epoch. A new epoch starts whenever guest memory may have changed in
/// a way later GPU work must see: a new guest submission, the end of a GPU wait on memory the
/// CPU or another queue writes, and command processor writes to guest memory. Within one epoch
/// the memory a submitted command buffer reads is stable, as it must be for the real GPU.
inline std::atomic<u32> g_upload_epoch{1};
/// EXP-126: graphics submissions decoded so far.
inline std::atomic<u64> g_gfx_submit_count{0};
/// PERF-049: told of each new epoch (the hot-page prehasher).
inline std::atomic<void (*)(u32)> g_upload_epoch_listener{nullptr};
/// DIAG-112: epoch bumps by source line (index line + 8192 for vk_rasterizer.cpp).
inline std::array<std::atomic<u32>, 16384> g_epoch_bump_sources{};
inline void BumpUploadEpoch(std::source_location loc = std::source_location::current()) {
    {
        const std::string_view file = loc.file_name();
        const u32 line = loc.line() & 8191;
        g_epoch_bump_sources[line + (file.find("rasterizer") != std::string_view::npos ? 8192 : 0)]
            .fetch_add(1, std::memory_order_relaxed);
    }
    const u32 epoch = g_upload_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    Common::GetWorkCounters().upload_epochs.fetch_add(1, std::memory_order_relaxed);
    if (const auto listener = g_upload_epoch_listener.load(std::memory_order_acquire)) {
        listener(epoch);
    }
}
constexpr u64 NUM_REGION_WORDS = HIGHER_PAGE_SIZE / BYTES_PER_WORD;

enum class Type : u8 {
    CPU = 1 << 0,
    GPU = 1 << 1,
};

enum class StateOp : u8 {
    None = 0,
    Set = 1,
    Clear = 2,
};

constexpr bool operator&(Type a, Type b) noexcept {
    return std::to_underlying(a) & std::to_underlying(b);
}

constexpr Type operator|(Type a, Type b) noexcept {
    return static_cast<Type>(std::to_underlying(a) | std::to_underlying(b));
}

struct Bounds {
    u64 start_word;
    u64 start_page;
    u64 end_word;
    u64 end_page;
};

struct RegionBits {
    constexpr void Fill(u64 value) {
        data.fill(value);
    }

    constexpr bool GetPage(u64 page) const {
        return data[page / PAGES_PER_WORD] & (1ULL << (page % PAGES_PER_WORD));
    }

    constexpr u64& operator[](u64 index) {
        return data[index];
    }

private:
    alignas(64) std::array<u64, NUM_REGION_WORDS> data;
};

} // namespace VideoCore
