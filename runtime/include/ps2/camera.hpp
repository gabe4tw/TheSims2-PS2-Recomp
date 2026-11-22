#pragma once

#include <cstdint>
#include <functional>
#include <vector>

// A camera for games that use a USB camera (Sony's EyeToy). The host (the runner) hands the runtime a source of pictures, for example
// a webcam of the PC; the runtime then plugs a virtual EyeToy into the console's first USB port (runtime/src/iop/usb.cpp, eyetoy.cpp)
// and turns the pictures into what the camera sends. With no source set the USB port stays empty, as before.
namespace ps2::camera {

struct Picture {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgb;  // width * height pixels, three bytes each (red, green, blue), top row first
};

// Fills `out` with the newest picture and returns true, or returns false while there is none (the camera still starts up, or it
// was unplugged). Called on the emulation thread, about thirty times a second while the game shows the camera picture.
using Source = std::function<bool(Picture &out)>;

// Set before the console starts (before ps2::system's reset): a camera is plugged in only when a source is set at that moment.
void set_source(Source source);
bool has_source();
bool latest(Picture &out);

}
