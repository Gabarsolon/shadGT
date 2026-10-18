#pragma once

namespace Ryty {

/// Forwards shadGT's controller state (keyboard bindings and gamepads) to Ryty's libScePad.
void StartInputBridge();

/// Applies the launcher's controller port choices; call before the window opens controllers.
void InstallControllerPorts();

} // namespace Ryty
