// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ryGT: AMD FSR 3.1 upscaling through AMD's FidelityFX API (amd_fidelityfx_vk.dll from FidelityFX
// SDK 1.1.4, the newest FSR with a Vulkan backend; MIT, headers in ryty_gpu/ffx_api). The DLL is
// loaded at run time from next to GTSport.exe when the GT Sport config has "fsr_enabled" (and
// "fsr3_enabled", on by default); without the DLL the renderer keeps using FSR 1.
//
// FSR 3.1 is a temporal upscaler, but the renderer only has the finished game picture, without
// motion vectors, depth or camera jitter. It runs with zero motion vectors, a flat depth image and
// its history reset every frame, which leaves FSR 3.1's spatial upsampling and RCAS sharpening.

#pragma once

#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan::RytyFsr3 {

/// Scales the game picture into an output image at the window size, like the FSR 1 pass.
class Pass {
public:
    ~Pass();

    void Create(vk::Instance instance, vk::PhysicalDevice physical_device, vk::Device device,
                VmaAllocator allocator, u32 num_images);

    /// Returns the scaled picture, or input when FSR 3.1 is off, unavailable or not upscaling.
    /// sharpness is FSR 3.1's 0..1 RCAS strength; negative turns sharpening off. pq: the picture
    /// is HDR10 (PQ) encoded.
    vk::ImageView Render(vk::CommandBuffer cmdbuf, vk::Image input_image, vk::ImageView input,
                         vk::Format input_format, vk::Extent2D input_size,
                         vk::Extent2D output_size, float sharpness, bool pq);

private:
    struct Img {
        u8 id{};
        bool dirty{true};
        bool swap_red_blue{}; // the output view swaps red and blue back (see Render)
        VideoCore::UniqueImage output_image;
        vk::UniqueImageView output_image_view;
    };

    bool Load();
    bool CreateContext(vk::Extent2D input_size, vk::Extent2D output_size);
    void DestroyContext();
    void CreateImages(Img& img) const;
    void CreateAuxImages(vk::CommandBuffer cmdbuf, vk::Extent2D input_size);

    vk::PhysicalDevice physical_device{};
    vk::Device device{};
    VmaAllocator allocator{};
    void* context{}; // ffxContext
    vk::Extent2D context_input{};
    vk::Extent2D context_output{};
    vk::Extent2D cur_size{};
    bool cur_swap_red_blue{};
    u32 cur_image{};
    std::vector<Img> available_imgs;
    // Zero motion vectors and a flat depth image at the game's size.
    VideoCore::UniqueImage motion_vectors;
    VideoCore::UniqueImage depth;
    vk::Extent2D aux_size{};
    bool failed{};
};

} // namespace Vulkan::RytyFsr3
