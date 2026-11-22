#pragma once

#include <cstdint>

namespace ps2::state {
class Stream;
}

// The IOP's USB host controller (an OHCI at 0x1F801600, interrupt line 22), emulated only when a camera is plugged in
// (ps2/camera.hpp); otherwise hw.cpp keeps answering as a controller with nothing attached, as it always did.
namespace ps2::iop::usb {

void reset();       // console start: plugs the camera in when a camera source is set
void soft_reset();  // IOP reboot
bool on();
std::uint32_t read(std::uint32_t a);
void write(std::uint32_t a, std::uint32_t v);
void state(ps2::state::Stream &s);
// After a save state was loaded (had_usb: it holds this controller; stub: the registers of the empty stand-in in hw.cpp): the
// camera is plugged in or pulled out to match this run, the way a player would do it on a console that is running.
void after_load(const std::uint32_t *stub, bool had_usb);

}
