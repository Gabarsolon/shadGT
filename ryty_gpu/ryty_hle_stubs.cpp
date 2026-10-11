// Pieces the renderer library provides that the other shadGT libraries only reference indirectly
// (through the emulator object and the debug state); they never run outside the renderer.

#include <string>

#include "common/types.h"
#include "core/debug_state.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

Frontend::WindowSDL::~WindowSDL() = default;
