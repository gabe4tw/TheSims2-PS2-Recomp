#pragma once

#include <string>
#include <vector>

// local change: the PC's webcam as the game's EyeToy (runtime ps2/camera.hpp). Windows Media Foundation capture on its own thread;
// the newest picture is handed to the runtime when the camera asks.
namespace webcam {

// The names of the video capture devices Windows knows, in its order (empty on other systems or with none).
std::vector<std::string> list();

// PS2_WEBCAM: "" or "none" = no camera (the USB port stays empty); "auto" = the first webcam; "test" = a moving test picture;
// anything else = the webcam with that name (the first one whose name contains it). Call before the console starts. Returns what
// was plugged in ("" for nothing).
std::string start(const std::string &choice);
void stop();

}
