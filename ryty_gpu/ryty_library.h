#pragma once

namespace Ryty {

/// Defined by each library's entry file: sets up the library and calls its RegisterLib.
void RytyLibraryInitialize();

/// Points every export at the function shadGT registered for its NID.
void BindExports();

/// Common setup for any library: logging to user/log/<log_name>, settings and game info.
void InitializeCommon(const char* log_name);

/// What the ImGui overlay layer draws each frame (the renderer's FPS counter).
void SetOverlayDraw(void (*draw)());

} // namespace Ryty
