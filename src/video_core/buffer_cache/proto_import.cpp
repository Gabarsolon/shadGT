// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <fmt/format.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "common/alignment.h"
#include "common/logging/log.h"
#include "video_core/buffer_cache/proto_import.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "core/memory.h"
#include <bit>
#include <chrono>

namespace VideoCore {

namespace {

std::string DescribeRegion(VAddr address) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0) {
        return "VirtualQuery failed";
    }
    const char* type = mbi.Type == MEM_PRIVATE ? "private"
                       : mbi.Type == MEM_MAPPED ? "mapped"
                       : mbi.Type == MEM_IMAGE  ? "image"
                                                : "?";
    return fmt::format("{} region {:#x}+{:#x} protect {:#x} state {:#x}",
                       type, reinterpret_cast<u64>(mbi.BaseAddress), mbi.RegionSize, mbi.Protect,
                       mbi.State);
#else
    return "n/a";
#endif
}

} // namespace

void ProtoImportTest(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, VAddr address,
                     u64 size) {
    if (!instance.IsExternalMemoryHostSupported()) {
        LOG_WARNING(Render_Vulkan, "P1: VK_EXT_external_memory_host is not available");
        return;
    }
    const auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    const VkDevice device = instance.GetDevice();
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                       &host_props};
    d.vkGetPhysicalDeviceProperties2(instance.GetPhysicalDevice(), &props2);
    const u64 align = host_props.minImportedHostPointerAlignment;
    VkPhysicalDeviceMemoryProperties mem_props{};
    d.vkGetPhysicalDeviceMemoryProperties(instance.GetPhysicalDevice(), &mem_props);
    LOG_WARNING(Render_Vulkan, "P1: import test at {:#x}+{:#x}, alignment {:#x}; {}", address,
                size, align, DescribeRegion(address));

    // GPU copy speed: 8 copies of the imported range into VRAM, against the same from VRAM.
    std::function<void(VkDeviceMemory, u64)> bandwidth_test;
    const auto make_buffer = [&](VkDeviceMemory memory, u64 length, bool external) {
        VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                             nullptr,
                                             VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, external ? &ext : nullptr};
        bi.size = length;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buffer{};
        d.vkCreateBuffer(device, &bi, nullptr, &buffer);
        d.vkBindBufferMemory(device, buffer, memory, 0);
        return buffer;
    };
    const auto device_local_memory = [&](u64 length) {
        VkDeviceMemory memory{};
        for (u32 i = 0; i < mem_props.memoryTypeCount; ++i) {
            if (mem_props.memoryTypes[i].propertyFlags == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, length, i};
                d.vkAllocateMemory(device, &info, nullptr, &memory);
                break;
            }
        }
        return memory;
    };
    const auto time_copies = [&](VkBuffer src, VkBuffer dst, u64 length) {
        scheduler.Finish();
        const auto start = std::chrono::steady_clock::now();
        scheduler.EndRendering();
        const vk::BufferCopy region{0, 0, length};
        for (int i = 0; i < 8; ++i) {
            scheduler.CommandBuffer().copyBuffer(vk::Buffer{src}, vk::Buffer{dst}, region);
        }
        scheduler.Finish();
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return 8.0 * double(length) / s / 1e9;
    };
    bool bandwidth_done = false;
    bandwidth_test = [&](VkDeviceMemory imported, u64 length) {
        if (bandwidth_done || length < 4_MB) {
            return;
        }
        bandwidth_done = true;
        const VkDeviceMemory vram_src = device_local_memory(length);
        const VkDeviceMemory vram_dst = device_local_memory(length);
        const VkBuffer host = make_buffer(imported, length, true);
        const VkBuffer src = make_buffer(vram_src, length, false);
        const VkBuffer dst = make_buffer(vram_dst, length, false);
        const double from_host = time_copies(host, dst, length);
        const double from_vram = time_copies(src, dst, length);
        const double from_host2 = time_copies(host, dst, length);
        LOG_WARNING(Render_Vulkan,
                    "P1: GPU copy of {:#x} bytes x8: from imported game memory {:.1f} / {:.1f} GB/s, "
                    "VRAM to VRAM {:.1f} GB/s (each includes one submit and wait)",
                    length, from_host, from_host2, from_vram);
        scheduler.Finish();
        for (VkBuffer b : {host, src, dst}) {
            d.vkDestroyBuffer(device, b, nullptr);
        }
        d.vkFreeMemory(device, vram_src, nullptr);
        d.vkFreeMemory(device, vram_dst, nullptr);
    };
    const auto try_import = [&](VAddr start, u64 length, const char* what) -> bool {
        start = Common::AlignDown(start, align);
        length = Common::AlignUp(length, align);
        VkMemoryHostPointerPropertiesEXT ptr_props{
            VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        const VkResult query = d.vkGetMemoryHostPointerPropertiesEXT(
            device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
            reinterpret_cast<void*>(start), &ptr_props);
        std::string types;
        u32 type_index = ~0u;
        for (u32 i = 0; i < mem_props.memoryTypeCount; ++i) {
            if (ptr_props.memoryTypeBits & (1u << i)) {
                types += fmt::format(" {}:{:#x}", i, u32(mem_props.memoryTypes[i].propertyFlags));
                if (type_index == ~0u) {
                    type_index = i;
                }
            }
        }
        VkResult alloc = VK_ERROR_UNKNOWN;
        VkResult bind = VK_ERROR_UNKNOWN;
        if (query == VK_SUCCESS && type_index != ~0u) {
            VkImportMemoryHostPointerInfoEXT import{
                VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, nullptr,
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                reinterpret_cast<void*>(start)};
            VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import, length,
                                      type_index};
            VkDeviceMemory memory{};
            alloc = d.vkAllocateMemory(device, &info, nullptr, &memory);
            if (alloc == VK_SUCCESS && bandwidth_test) {
                bandwidth_test(memory, length);
            }
            if (alloc == VK_SUCCESS) {
                VkExternalMemoryBufferCreateInfo external{
                    VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, nullptr,
                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
                VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
                buffer_info.size = length;
                buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                VkBuffer buffer{};
                if (d.vkCreateBuffer(device, &buffer_info, nullptr, &buffer) == VK_SUCCESS) {
                    bind = d.vkBindBufferMemory(device, buffer, memory, 0);
                    d.vkDestroyBuffer(device, buffer, nullptr);
                }
                d.vkFreeMemory(device, memory, nullptr);
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "P1: import {} {:#x}+{:#x}: pointer query {}, types{}, allocate {}, bind {}",
                    what, start, length, int(query), types, int(alloc), int(bind));
        return alloc == VK_SUCCESS;
    };
#ifdef _WIN32
    // The same with the range made writable for the import and its protections put back after.
    const auto try_writable = [&](VAddr start, u64 length, const char* what) {
        start = Common::AlignDown(start, align);
        length = Common::AlignUp(length, align);
        std::vector<std::pair<VAddr, std::pair<u64, DWORD>>> saved;
        for (VAddr cursor = start; cursor < start + length;) {
            MEMORY_BASIC_INFORMATION run{};
            VirtualQuery(reinterpret_cast<void*>(cursor), &run, sizeof(run));
            const u64 bytes = std::min<u64>(run.RegionSize - (cursor - reinterpret_cast<VAddr>(run.BaseAddress)),
                                            start + length - cursor);
            saved.push_back({cursor, {bytes, run.Protect}});
            cursor += bytes;
        }
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(start), length, PAGE_READWRITE, &old)) {
            LOG_WARNING(Render_Vulkan, "P1: VirtualProtect RW failed: {}", GetLastError());
        }
        const std::string label = std::string(what) + " (made writable)";
        try_import(start, length, label.c_str());
        for (const auto& [address, run] : saved) {
            VirtualProtect(reinterpret_cast<void*>(address), run.first, run.second, &old);
        }
    };
#endif
    try_import(address, 16_KB, "16 KB");
    try_import(address, 64_KB, "64 KB");
#ifdef _WIN32
    // The view (allocation) holding the address, and its protection runs.
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi));
    const VAddr view_base = reinterpret_cast<VAddr>(mbi.AllocationBase);
    VAddr cursor = view_base;
    std::string runs;
    u32 count = 0;
    while (true) {
        MEMORY_BASIC_INFORMATION run{};
        if (VirtualQuery(reinterpret_cast<void*>(cursor), &run, sizeof(run)) == 0 ||
            run.AllocationBase != mbi.AllocationBase) {
            break;
        }
        if (count++ < 12) {
            runs += fmt::format(" +{:#x}:{:#x}/{:#x}", cursor - view_base, run.RegionSize,
                                run.Protect);
        }
        cursor += run.RegionSize;
    }
    LOG_WARNING(Render_Vulkan, "P1: view {:#x}+{:#x} type {:#x} allocation protect {:#x} ({} "
                "protection runs):{}", view_base, cursor - view_base, u32(mbi.Type),
                u32(mbi.AllocationProtect), count, runs);
    try_import(view_base, std::min<u64>(cursor - view_base, 64_KB), "view start 64 KB");
    try_import(view_base, std::min<u64>(cursor - view_base, 256_MB), "whole view");
    try_writable(address, 16_KB, "16 KB");
    try_writable(view_base, std::min<u64>(cursor - view_base, 256_MB), "whole view");
#endif
    // Pinned test: the view imported while writable, its protections put back, then the GPU's
    // copy of it compared with the CPU's bytes; the import stays alive for the rest of the run.
    {
        const VAddr start = view_base;
        const u64 length = std::min<u64>(cursor - view_base, 64_MB);
        std::vector<std::pair<VAddr, std::pair<u64, DWORD>>> saved;
        for (VAddr at = start; at < start + length;) {
            MEMORY_BASIC_INFORMATION run{};
            VirtualQuery(reinterpret_cast<void*>(at), &run, sizeof(run));
            const u64 bytes = std::min<u64>(
                run.RegionSize - (at - reinterpret_cast<VAddr>(run.BaseAddress)), start + length - at);
            saved.push_back({at, {bytes, run.Protect}});
            at += bytes;
        }
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(start), length, PAGE_READWRITE, &old);
        VkImportMemoryHostPointerInfoEXT import{
            VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, nullptr,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, reinterpret_cast<void*>(start)};
        VkMemoryHostPointerPropertiesEXT ptr_props{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        d.vkGetMemoryHostPointerPropertiesEXT(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                              reinterpret_cast<void*>(start), &ptr_props);
        const u32 type_index = std::countr_zero(ptr_props.memoryTypeBits);
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import, length, type_index};
        VkDeviceMemory pinned{};
        const VkResult alloc = d.vkAllocateMemory(device, &info, nullptr, &pinned);
        // SHADGT_PROTO_RESTORE=1 puts the protections back (that lost the device on 2026-10-15).
        if (const char* env = std::getenv("SHADGT_PROTO_RESTORE"); env && env[0] == '1') {
            for (const auto& [at, run] : saved) {
                VirtualProtect(reinterpret_cast<void*>(at), run.first, run.second, &old);
            }
        }
        if (alloc == VK_SUCCESS) {
            // Read back through the GPU into a host-visible buffer (the import's own type).
            void* readback_host = VirtualAlloc(nullptr, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            VkImportMemoryHostPointerInfoEXT rb_import{
                VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, nullptr,
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, readback_host};
            VkMemoryAllocateInfo rb_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &rb_import, length,
                                         type_index};
            VkDeviceMemory rb_memory{};
            d.vkAllocateMemory(device, &rb_info, nullptr, &rb_memory);
            const VkBuffer src = make_buffer(pinned, length, true);
            const VkBuffer dst = make_buffer(rb_memory, length, true);
            scheduler.EndRendering();
            scheduler.CommandBuffer().copyBuffer(vk::Buffer{src}, vk::Buffer{dst},
                                                 vk::BufferCopy{0, 0, length});
            scheduler.Finish();
            u64 differing = 0;
            const auto* gpu = static_cast<const u8*>(readback_host);
            std::vector<u8> cpu(4096);
            for (u64 off = 0; off < length; off += 4096) {
                Core::Memory::Instance()->CopySparseMemory(start + off, cpu.data(), 4096);
                differing += std::memcmp(cpu.data(), gpu + off, 4096) != 0;
            }
            LOG_WARNING(Render_Vulkan,
                        "P1: pinned {:#x}+{:#x} with protections back: GPU copy differs from the "
                        "CPU bytes on {} of {} pages (the game writes meanwhile); kept imported",
                        start, length, differing, length / 4096);
        } else {
            LOG_WARNING(Render_Vulkan, "P1: pinned import failed: {}", int(alloc));
        }
    }
    // Control cases: memory of known kinds.
    {
        void* priv = VirtualAlloc(nullptr, 1_MB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        std::memset(priv, 1, 1_MB);
        try_import(reinterpret_cast<VAddr>(priv), 1_MB, "control VirtualAlloc 1 MB");
        VirtualFree(priv, 0, MEM_RELEASE);
        for (const DWORD section_prot : {DWORD(PAGE_READWRITE), DWORD(PAGE_EXECUTE_READWRITE)}) {
            HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, section_prot, 0,
                                                DWORD(4_MB), nullptr);
            void* view = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, 1_MB);
            if (view) {
                std::memset(view, 1, 1_MB);
                try_import(reinterpret_cast<VAddr>(view), 1_MB,
                           section_prot == PAGE_READWRITE ? "control section RW view 1 MB"
                                                          : "control section RWX view 1 MB");
                UnmapViewOfFile(view);
            }
            CloseHandle(section);
        }
    }
    LOG_WARNING(Render_Vulkan, "P1: maxMemoryAllocationCount {}",
                props2.properties.limits.maxMemoryAllocationCount);
}

VAddr GuestViewBase(VAddr address) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0) {
        return 0;
    }
    return reinterpret_cast<VAddr>(mbi.AllocationBase);
#else
    return 0;
#endif
}

VAddr GuestViewEnd(VAddr base, u64 max_size) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(base), &mbi, sizeof(mbi)) == 0) {
        return 0;
    }
    VAddr end = base;
    while (end - base <= max_size) {
        MEMORY_BASIC_INFORMATION run{};
        if (VirtualQuery(reinterpret_cast<void*>(end), &run, sizeof(run)) == 0 ||
            run.AllocationBase != mbi.AllocationBase || run.State != MEM_COMMIT) {
            break;
        }
        end += run.RegionSize;
    }
    return end;
#else
    return 0;
#endif
}

vk::Buffer ImportGuestView(const Vulkan::Instance& instance, VAddr address, u64 size, u64 max_size,
                           VAddr* view_begin, VAddr* view_end) {
#ifdef _WIN32
    if (!instance.IsExternalMemoryHostSupported()) {
        return {};
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0 ||
        mbi.State != MEM_COMMIT) {
        return {};
    }
    const VAddr base = reinterpret_cast<VAddr>(mbi.AllocationBase);
    VAddr end = base;
    while (end - base <= max_size) {
        MEMORY_BASIC_INFORMATION run{};
        if (VirtualQuery(reinterpret_cast<void*>(end), &run, sizeof(run)) == 0 ||
            run.AllocationBase != mbi.AllocationBase || run.State != MEM_COMMIT) {
            break;
        }
        end += run.RegionSize;
    }
    if (end - base > max_size || address + size > end || (base & 0xFFF) != 0) {
        static std::atomic<u32> logged{};
        if (logged++ < 20) {
            LOG_WARNING(Render_Vulkan, "P2: view {:#x} not imported: {:#x}+{:#x} reaches {:#x} (view "
                        "end {:#x}, max {:#x})", base, address, size, address + size, end, max_size);
        }
        return {};
    }
    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void*>(base), end - base, PAGE_READWRITE, &old)) {
        return {};
    }
    const auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    const VkDevice device = instance.GetDevice();
    VkMemoryHostPointerPropertiesEXT ptr_props{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (d.vkGetMemoryHostPointerPropertiesEXT(device,
                                              VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                              reinterpret_cast<void*>(base), &ptr_props) != VK_SUCCESS ||
        ptr_props.memoryTypeBits == 0) {
        return {};
    }
    VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            nullptr,
                                            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            reinterpret_cast<void*>(base)};
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import, end - base,
                              static_cast<u32>(std::countr_zero(ptr_props.memoryTypeBits))};
    VkDeviceMemory memory{};
    if (const VkResult r = d.vkAllocateMemory(device, &info, nullptr, &memory); r != VK_SUCCESS) {
        LOG_WARNING(Render_Vulkan, "P2: import of view {:#x}+{:#x} failed: {}", base, end - base,
                    int(r));
        return {};
    }
    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                              nullptr,
                                              VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
    buffer_info.size = end - base;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                        VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    VkBuffer buffer{};
    if (d.vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS ||
        d.vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
        return {};
    }
    *view_begin = base;
    *view_end = end;
    return vk::Buffer{buffer};
#else
    return {};
#endif
}

} // namespace VideoCore
