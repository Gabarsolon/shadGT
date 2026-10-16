// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace VideoCore {

struct Buffer;

/// D1 (direct memory model, SHADGT_DIRECT=1): the GPU reads guest memory itself.
///
/// Each guest view (a run of one memory section, as Ryty maps it) gets a writable alias view
/// (GuestArenaMapAlias) that is imported into Vulkan (VK_EXT_external_memory_host). The game's
/// own view keeps whatever protection the renderer's write tracking or the game sets; the GPU
/// goes through the alias, which never changes. A two-level page table on the GPU maps each
/// 16 KiB guest page to the device address of its bytes:
///   L1[address >> 25]           -> device address of an L2 table (0: nothing mapped there)
///   L2[(address >> 14) & 2047]  -> device address of the page (0: not imported)
/// Shaders read buffers through it, so no CPU work is needed to make guest bytes visible.
class DirectMemory {
public:
    static constexpr u32 PageBits = 14;
    static constexpr u64 PageSize = u64{1} << PageBits;
    static constexpr u32 L2Bits = 11; // 2048 pages = 32 MiB per L1 entry
    static constexpr u32 L1Bits = 40 - PageBits - L2Bits;
    static constexpr u64 L1Entries = u64{1} << L1Bits;
    static constexpr u64 L2Entries = u64{1} << L2Bits;
    static constexpr u32 MaxL2Tables = 4096; // 128 GiB of guest address space

    explicit DirectMemory(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler);
    ~DirectMemory();

    /// Whether the model is on (SHADGT_DIRECT=1 and the driver can import host memory).
    static bool Enabled();

    /// Imports the views covering [address, address + size) not imported yet. True when every
    /// page of the range is now mapped for the GPU.
    bool EnsureImported(VAddr address, u64 size);

    /// Whether every page of the range is mapped for the GPU (no import attempted).
    bool Covered(VAddr address, u64 size) const;

    /// Ryty unmaps guest memory (before it does): the pages leave the page table; the aliases are
    /// released once the GPU has finished the work recorded so far.
    void OnUnmap(VAddr address, u64 size);

    /// Device address of the L1 table, for the shaders.
    vk::DeviceAddress L1Address() const;

    struct Stats {
        u64 views;
        u64 bytes;
        u64 failed;
    };
    Stats GetStats() const;

private:
    struct View {
        VAddr begin;
        VAddr end;
        void* alias;
        VkDeviceMemory memory;
        VkBuffer buffer;
        vk::DeviceAddress device_address;
    };
    bool ImportView(VAddr address);
    void MapPages(VAddr begin, VAddr end, vk::DeviceAddress device_address);
    void UnmapPages(VAddr begin, VAddr end);
    u64* L2For(VAddr address, bool create);
    void Release(View& view);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    mutable std::mutex mutex;
    std::map<VAddr, View> views; // by begin
    std::vector<VAddr> failed;   // view bases that could not be imported (sorted)
    std::unique_ptr<Buffer> l1;
    std::unique_ptr<Buffer> l2_pool;
    u32 l2_used{};
    std::vector<u32> l2_index; // L1 entry -> L2 table + 1 (0: none)
    std::atomic<u64> imported_bytes{};
    std::atomic<u64> failed_count{};
};

} // namespace VideoCore
