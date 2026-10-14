// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {
class Instance;
}

namespace Vulkan::RytyFrameGen {

/// ryGT: AMD FSR 3.1 frame generation (amd_fidelityfx_vk.dll, GPU.framegen_enabled). FidelityFX
/// replaces the swapchain with its own, which shows a generated frame between each pair of game
/// frames from its own present thread. The game picture comes without motion vectors or depth,
/// so the generated frames come from FSR's optical flow alone.
class FrameGen {
public:
    ~FrameGen();

    /// Creates the swapchain through FidelityFX. False when frame generation is off or
    /// unavailable; the caller then creates a normal swapchain.
    bool CreateSwapchain(const Instance& instance, const vk::SwapchainCreateInfoKHR& info,
                         vk::SwapchainKHR& swapchain);
    /// Destroys the swapchain and both FidelityFX contexts (the device must be idle).
    void Destroy();

    bool Active() const {
        return swapchain_context != nullptr;
    }

    vk::Result GetImages(vk::Device device, vk::SwapchainKHR swapchain,
                         std::vector<vk::Image>& images) const;
    vk::Result Acquire(vk::Device device, vk::SwapchainKHR swapchain, vk::Semaphore semaphore,
                       u32* image_index) const;
    vk::Result Present(vk::Queue queue, const vk::PresentInfoKHR& info) const;

    /// Per presented game frame, before the frame's command buffer is submitted: configures
    /// frame generation and records its preparation.
    void Prepare(vk::CommandBuffer cmdbuf, vk::SwapchainKHR swapchain, vk::Extent2D size,
                 bool hdr);

private:
    void CreateAuxImages(vk::CommandBuffer cmdbuf, vk::Extent2D size);

    vk::Device device{};
    VmaAllocator allocator{};
    void* swapchain_context{};
    void* framegen_context{};
    void* create_swapchain{};
    void* destroy_swapchain{};
    PFN_vkGetSwapchainImagesKHR get_images{};
    PFN_vkAcquireNextImageKHR acquire{};
    PFN_vkQueuePresentKHR present{};
    void* present_count{}; // PFN_getLastPresentCountFFXAPI
    u64 logged_presents{};
    u64 logged_frames{};
    std::chrono::steady_clock::time_point last_log{};
    vk::Extent2D display_size{};
    vk::Format format{};
    bool hdr_output{};
    u64 frame_id{};
    std::chrono::steady_clock::time_point last_prepare{};
    vk::Extent2D aux_size{};
    VideoCore::UniqueImage motion_vectors;
    VideoCore::UniqueImage depth;
};

/// True when GPU.framegen_enabled is on (the renderer then asks for extra queues).
bool Requested();

/// Frames shown so far (game and generated), for the FPS counter; 0 while generation is off.
u64 ShownFrames();

} // namespace Vulkan::RytyFrameGen
