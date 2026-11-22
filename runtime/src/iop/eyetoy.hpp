#pragma once

#include <cstdint>
#include <vector>

namespace ps2::state {
class Stream;
}

// A virtual Sony EyeToy (USB 054C:0155, OmniVision OV519 bridge with an OV7648 sensor) behind the USB host controller of usb.cpp.
// What it has to do was worked out from Sony's own IOP driver (usbcam, "EyeToy", packed in the Sims 2 disc's IOPSKU2.DAT) and the
// EE side of libeyety in the game, together with the public USB 1.1 and MPEG specifications:
// - usbcam's probe takes vendor 054C, products 0154 to 0157; its connect wants a first interface with one isochronous IN endpoint
//   and picks the alternate setting whose endpoint carries 896 bytes (alternates of 0, 384, 512, 768 and 896 bytes).
// - the bridge's registers are read and written with vendor request 1 (0xC0 / 0x40, wIndex = register, one data byte); the sensor
//   is reached through the bridge's I2C registers: 0x42 = register to write, 0x43 = register to read, 0x45 = data, 0x47 = command
//   (1 = write, 3 then 5 = read). Register 0x10 is the picture width / 16, 0x11 the height / 8.
// - pictures arrive as isochronous packets: one starting with a 16-byte header ff ff ff 50 (start of frame), data packets, and a
//   packet holding only a 16-byte header ff ff ff 51 (end of frame; its byte 9 must be 0). usbcam drops a frame over 20480 bytes.
// - the frame is an IPU picture: "ipum", total size (header included, a multiple of 16), width, height (16 bits each), 4 zero
//   bytes, then a byte the EE puts into IPU_CTRL bits 16-23 (bit 2 of it: dct_type present) and an MPEG intra macroblock stream
//   that libeyety decodes with one IPU IDEC command (quantiser scale 1, RGB16 out), ended by a start code.
namespace ps2::iop::eyetoy {

void reset();
std::uint8_t address();

// The control endpoint. setup() starts a request (false: the device stalls it), in() hands out its answer, out() takes its data,
// status() is the status stage (a request with OUT data is done there). Return values are byte counts, -1 for a stall.
bool setup(const std::uint8_t packet[8]);
int in(std::uint8_t *dst, int max);
int out(const std::uint8_t *src, int n);
void status();

// One isochronous IN packet of endpoint 1 (called once per USB frame, 1 ms, while the game streams). -1 when nothing listens.
int iso_in(int endpoint, std::uint8_t *dst, int max);

void state(ps2::state::Stream &s);

// The picture encoder on its own, for tests: rgb is w * h * 3 bytes; the result is a whole "ipum" frame.
std::vector<std::uint8_t> encode(const std::uint8_t *rgb, int w, int h, int qscale);

}
