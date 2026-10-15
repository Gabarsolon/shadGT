// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
}

namespace VideoCore {

/// P1 (SHADGT_PROTO_IMPORT=1, prototype): tries once to import the guest memory around a range
/// the recorder synchronizes into Vulkan (VK_EXT_external_memory_host) at several sizes and logs
/// what the driver says.
void ProtoImportTest(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, VAddr address,
                     u64 size);

/// P2 (SHADGT_PROTO_INPLACE=1, prototype): guest views imported into Vulkan. The GPU reads and
/// writes them in place: the renderer never write-protects them (the GPU faults on protected
/// imported pages) and does not track their changes.
struct ImportedView {
    VAddr begin;
    VAddr end;
    const void* buffer; // VideoCore::Buffer
};
inline std::shared_mutex g_imported_mutex;
inline std::vector<ImportedView> g_imported_views; // sorted by begin
inline std::atomic<u32> g_imported_count{};

/// The imported view holding all of [address, address + size), or null.
inline const ImportedView* FindImportedView(VAddr address, u64 size) {
    if (g_imported_count.load(std::memory_order_acquire) == 0) {
        return nullptr;
    }
    std::shared_lock lk{g_imported_mutex};
    for (const auto& view : g_imported_views) {
        if (address >= view.begin && address + size <= view.end) {
            return &view;
        }
    }
    return nullptr;
}

/// Calls f(begin, size) for the parts of the range outside imported views.
template <typename F>
void ForEachNotImported(VAddr address, u64 size, F&& f) {
    if (g_imported_count.load(std::memory_order_acquire) == 0) {
        f(address, size);
        return;
    }
    std::shared_lock lk{g_imported_mutex};
    VAddr cursor = address;
    const VAddr end = address + size;
    for (const auto& view : g_imported_views) {
        if (view.end <= cursor || view.begin >= end) {
            continue;
        }
        if (view.begin > cursor) {
            f(cursor, view.begin - cursor);
        }
        cursor = std::max(cursor, view.end);
    }
    if (cursor < end) {
        f(cursor, end - cursor);
    }
}

/// The base of the guest view (allocation) holding the address, or 0.
VAddr GuestViewBase(VAddr address);
/// The end of that view (walked up to max_size), or 0.
VAddr GuestViewEnd(VAddr base, u64 max_size);

/// Imports the guest view holding [address, address + size) when it is at most max_size and
/// mapped memory; makes it writable first. The Vulkan buffer over it, or null.
vk::Buffer ImportGuestView(const Vulkan::Instance& instance, VAddr address, u64 size, u64 max_size,
                           VAddr* view_begin, VAddr* view_end);

} // namespace VideoCore
