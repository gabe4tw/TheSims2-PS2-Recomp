#include "usb.hpp"

#include "eyetoy.hpp"
#include "hw.hpp"
#include "iop.hpp"
#include "kernel.hpp"

#include "ps2/camera.hpp"
#include "ps2/state.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// An OHCI 1.0a host controller (Open Host Controller Interface Specification, Release 1.0a, public) with a two-port root hub, as
// much of it as Sony's usbd needs: the operational registers, the root hub ports, the control, bulk and periodic (interrupt and
// isochronous) lists, the done queue in the HCCA and the frame counter. Each 1 ms frame is one job of the IOP's event queue
// (kJobUsbFrame); within a frame every list is walked once and every transfer finishes at once (a device here never NAKs).
// One device can be attached: the EyeToy of eyetoy.cpp, on the first port.
namespace ps2::iop::usb {

namespace {

constexpr unsigned kLine = 22;                       // the IOP's USB interrupt
constexpr std::uint64_t kFrameCycles = 36864;        // 1 ms of IOP time
enum : unsigned {
    kRevision, kControl, kCommandStatus, kInterruptStatus, kInterruptEnable, kInterruptDisable, kHcca, kPeriodCurrentEd,
    kControlHeadEd, kControlCurrentEd, kBulkHeadEd, kBulkCurrentEd, kDoneHead, kFmInterval, kFmRemaining, kFmNumber,
    kPeriodicStart, kLsThreshold, kRhDescriptorA, kRhDescriptorB, kRhStatus, kRhPort0, kRhPort1, kRegCount = 0x18
};
constexpr std::uint32_t kWdh = 2, kSf = 4, kRhsc = 0x40, kMie = 0x80000000u;
constexpr std::uint32_t kCcs = 1, kPes = 2, kPrs = 0x10, kPps = 0x100, kCsc = 0x10000, kPesc = 0x20000, kPrsc = 0x100000;
enum : std::uint32_t { kCcNoError = 0, kCcStall = 4, kCcNotResponding = 5, kCcDataOverrun = 8, kCcDataUnderrun = 9, kCcNotAccessed = 15 };

struct State {
    std::uint32_t on = 0;
    std::uint32_t regs[kRegCount] = {};
    std::uint32_t ticking = 0;   // a frame job is queued
    std::uint32_t done = 0;      // the done queue not yet written to the HCCA
    std::uint32_t generation = 0;
};
State g;
bool g_trace = false;

std::uint32_t r32(std::uint32_t a) {
    std::uint32_t v;
    std::memcpy(&v, ram_ptr(a & ~3u), 4);
    return v;
}
void w32(std::uint32_t a, std::uint32_t v) { std::memcpy(ram_ptr(a & ~3u), &v, 4); }
std::uint16_t r16(std::uint32_t a) {
    std::uint16_t v;
    std::memcpy(&v, ram_ptr(a & ~1u), 2);
    return v;
}
void w16(std::uint32_t a, std::uint16_t v) { std::memcpy(ram_ptr(a & ~1u), &v, 2); }

void update_interrupt() {
    const std::uint32_t pending = g.regs[kInterruptStatus] & g.regs[kInterruptEnable] & 0x7FFFFFFFu;
    if (pending && (g.regs[kInterruptEnable] & kMie)) hw::raise_interrupt(kLine);
}

void set_status(std::uint32_t bits) {
    g.regs[kInterruptStatus] |= bits;
    update_interrupt();
}

bool operational() { return ((g.regs[kControl] >> 6) & 3u) == 2u; }

// A transfer buffer of a general TD: from cbp to be, which may cross into the page of be once.
struct Buffer {
    std::uint32_t cbp, be;
    std::uint32_t length() const {
        if (!cbp) return 0;
        if ((cbp ^ be) & ~0xFFFu) return (0x1000u - (cbp & 0xFFFu)) + (be & 0xFFFu) + 1u;
        return be - cbp + 1u;
    }
    std::uint32_t at(std::uint32_t offset) const {
        const std::uint32_t first = 0x1000u - (cbp & 0xFFFu);
        if (((cbp ^ be) & ~0xFFFu) && offset >= first) return (be & ~0xFFFu) + (offset - first);
        return cbp + offset;
    }
    void read(std::uint32_t offset, std::uint8_t *dst, std::uint32_t n) const {
        for (std::uint32_t i = 0; i < n; ++i) dst[i] = *ram_ptr(at(offset + i));
    }
    void write(std::uint32_t offset, const std::uint8_t *src, std::uint32_t n) const {
        for (std::uint32_t i = 0; i < n; ++i) *ram_ptr(at(offset + i)) = src[i];
    }
};

bool device_here(std::uint32_t function) {
    return (g.regs[kRhPort0] & (kCcs | kPes)) == (kCcs | kPes) && function == eyetoy::address();
}

void retire(std::uint32_t td) {
    w32(td + 8, g.done);
    g.done = td;
}

// The general TDs of one endpoint (control, bulk or interrupt): all of them, until the ED's queue is empty or it halts.
void run_general(std::uint32_t ed) {
    const std::uint32_t info = r32(ed);
    const std::uint32_t function = info & 0x7Fu, endpoint = (info >> 7) & 0xFu, ed_dir = (info >> 11) & 3u;
    const std::uint32_t mps = (info >> 16) & 0x7FFu;
    for (int guard = 0; guard < 64; ++guard) {
        std::uint32_t head = r32(ed + 8);
        const std::uint32_t tail = r32(ed + 4) & ~0xFu;
        if ((head & 1u) || (head & ~0xFu) == tail) return;
        const std::uint32_t td = head & ~0xFu;
        std::uint32_t control = r32(td);
        const Buffer buf{r32(td + 4), r32(td + 12)};
        const std::uint32_t pid = (ed_dir == 1 || ed_dir == 2) ? ed_dir : (control >> 19) & 3u;  // 0 setup, 1 out, 2 in
        const std::uint32_t length = buf.length();
        std::uint32_t cc = kCcNoError, moved = length;
        if (!device_here(function)) {
            cc = kCcNotResponding;
            moved = 0;
        } else if (pid == 0) {
            std::uint8_t packet[8] = {};
            buf.read(0, packet, length < 8 ? length : 8);
            if (!eyetoy::setup(packet)) cc = kCcStall;
        } else if (endpoint != 0) {
            cc = kCcStall;  // the camera's only other endpoint is isochronous
            moved = 0;
        } else if (pid == 2) {
            if (length == 0) {
                eyetoy::status();
            } else {
                std::uint8_t data[4096];
                const int n = eyetoy::in(data, static_cast<int>(length < sizeof data ? length : sizeof data));
                moved = n < 0 ? 0 : static_cast<std::uint32_t>(n);
                buf.write(0, data, moved);
                if (n < 0) cc = kCcStall;
                else if (moved < length && !((control >> 18) & 1u)) cc = kCcDataUnderrun;
            }
        } else {
            if (length == 0) {
                eyetoy::status();
            } else {
                std::uint8_t data[4096];
                const std::uint32_t n = length < sizeof data ? length : sizeof data;
                buf.read(0, data, n);
                eyetoy::out(data, static_cast<int>(n));
            }
        }
        (void)mps;
        if (g_trace) std::fprintf(stderr, "[usb] td 0x%06X fn %u ep %u pid %u len %u -> cc %u moved %u\n", td, function, endpoint, pid, length, cc, moved);
        // the data toggle: from the TD when its T bit 1 is set, else the ED's carry; one packet's worth flips it per packet
        const std::uint32_t packets = mps ? (moved + mps - 1) / mps : 1;
        std::uint32_t toggle = (control & 0x2000000u) ? (control >> 24) & 1u : (head >> 1) & 1u;
        if (cc == kCcNoError || cc == kCcDataUnderrun) toggle ^= (packets ? packets : 1) & 1u;
        control = (control & 0x00FFFFFFu) | (cc << 28) | 0x2000000u | (toggle << 24);
        w32(td, control);
        if (cc == kCcNoError) w32(td + 4, 0);
        else if (moved) w32(td + 4, buf.at(moved));
        const bool halt = cc != kCcNoError;
        const std::uint32_t next = r32(td + 8) & ~0xFu;
        w32(ed + 8, next | (toggle << 1) | (halt ? 1u : 0u));
        retire(td);
        if (halt) return;
    }
}

// The isochronous TD at the head of an ED: one packet per frame, the one whose frame number has come.
void run_iso(std::uint32_t ed) {
    const std::uint32_t info = r32(ed);
    const std::uint32_t function = info & 0x7Fu, endpoint = (info >> 7) & 0xFu;
    for (int guard = 0; guard < 8; ++guard) {
        const std::uint32_t head = r32(ed + 8);
        const std::uint32_t tail = r32(ed + 4) & ~0xFu;
        if ((head & 1u) || (head & ~0xFu) == tail) return;
        const std::uint32_t td = head & ~0x1Fu;
        const std::uint32_t control = r32(td);
        const std::uint32_t sf = control & 0xFFFFu, fc = (control >> 24) & 7u;
        const std::int16_t relative = static_cast<std::int16_t>(static_cast<std::uint16_t>(g.regs[kFmNumber]) - sf);
        if (relative < 0) return;
        const std::uint32_t bp0 = r32(td + 4) & ~0xFFFu, be = r32(td + 12);
        auto offset = [&](std::uint32_t i) { return r16(td + 16 + 2 * i); };
        auto address = [&](std::uint32_t off) { return ((off & 0x1000u) ? (be & ~0xFFFu) : bp0) | (off & 0xFFFu); };
        bool finished = false;
        if (static_cast<std::uint32_t>(relative) > fc) {
            // too late for the rest of it: the packets not done keep the "not accessed" code the driver put in their offset words
            if (g_trace) std::fprintf(stderr, "[usb] iso td 0x%06X late: frame %u, sf %u, %u packets\n", td, g.regs[kFmNumber], sf, fc + 1);
            w32(td, (control & 0x0FFFFFFFu) | (kCcDataOverrun << 28));
            finished = true;
        } else {
            const std::uint32_t i = static_cast<std::uint32_t>(relative);
            const std::uint32_t start = address(offset(i));
            const std::uint32_t end = i < fc ? address(offset(i + 1)) - 1 : be;
            const std::uint32_t room = end >= start ? end - start + 1 : 0;
            int n = -1;
            std::uint8_t data[1024];
            if (device_here(function)) n = eyetoy::iso_in(static_cast<int>(endpoint), data, static_cast<int>(room < sizeof data ? room : sizeof data));
            std::uint32_t psw;
            if (n < 0) {
                psw = kCcNotResponding << 12;
            } else {
                for (int k = 0; k < n; ++k) *ram_ptr(start + static_cast<std::uint32_t>(k)) = data[k];
                psw = (kCcNoError << 12) | static_cast<std::uint32_t>(n);
            }
            w16(td + 16 + 2 * i, static_cast<std::uint16_t>(psw));
            if (g_trace && endpoint == 1 && n > 0) {
                static int shown = 0;
                if (shown++ < 400)
                    std::fprintf(stderr, "[usb] iso frame %u td 0x%06X sf %u packet %u/%u at 0x%06X room %u -> %d bytes %02X%02X%02X%02X\n", g.regs[kFmNumber], td, sf, i,
                                 fc, start, room, n, data[0], data[1], data[2], data[3]);
            }
            if (i == fc) {
                w32(td, (control & 0x0FFFFFFFu) | (kCcNoError << 28));
                finished = true;
            }
        }
        if (!finished) return;
        w32(ed + 8, (r32(td + 8) & ~0x1Fu) | (head & 2u));
        retire(td);
        if (static_cast<std::uint32_t>(relative) <= fc) return;  // one packet per frame
    }
}

void run_list(std::uint32_t ed, bool periodic) {
    for (int guard = 0; ed && guard < 256; ++guard) {
        ed &= ~0xFu;
        const std::uint32_t info = r32(ed);
        if (!(info & 0x4000u)) {  // not skipped
            if (info & 0x8000u) {
                if (periodic && (g.regs[kControl] & 8u)) run_iso(ed);
            } else {
                run_general(ed);
            }
        }
        ed = r32(ed + 12);
    }
}

void schedule_frame() {
    if (g.ticking || !operational()) return;
    g.ticking = 1;
    kernel::schedule(kernel::now() + kFrameCycles, kernel::Job{kernel::kJobUsbFrame, g.generation});
}

void frame(const kernel::Job &job) {
    if (job.a != g.generation) return;
    g.ticking = 0;
    if (!g.on || !operational()) return;
    g.regs[kFmNumber] = (g.regs[kFmNumber] + 1) & 0xFFFFu;
    const std::uint32_t hcca = g.regs[kHcca] & ~0xFFu;
    if (hcca) w16(hcca + 0x80, static_cast<std::uint16_t>(g.regs[kFmNumber]));
    std::uint32_t status = kSf;
    if (hcca && (g.regs[kControl] & 4u)) run_list(r32(hcca + 4 * (g.regs[kFmNumber] & 31u)), true);
    if (g.regs[kControl] & 0x10u) run_list(g.regs[kControlHeadEd], false);
    if (g.regs[kControl] & 0x20u) run_list(g.regs[kBulkHeadEd], false);
    if (g.done && !(g.regs[kInterruptStatus] & kWdh) && hcca) {
        const bool others = (g.regs[kInterruptStatus] & g.regs[kInterruptEnable] & ~kWdh) != 0;
        w32(hcca + 0x84, g.done | (others ? 1u : 0u));
        g.done = 0;
        status |= kWdh;
    }
    set_status(status);
    schedule_frame();
}

void controller_reset() {
    const std::uint32_t on = g.on, generation = g.generation + 1;
    g = State{};
    g.on = on;
    g.generation = generation;
    g.regs[kRevision] = 0x110u;
    g.regs[kFmInterval] = 0x27782EDFu;
    g.regs[kLsThreshold] = 0x628u;
    g.regs[kRhDescriptorA] = 0x00000202u;  // two ports, always powered
    g.regs[kRhPort0] = g.regs[kRhPort1] = kPps;
    if (g.on) g.regs[kRhPort0] |= kCcs | kCsc;
    eyetoy::reset();
}

}

void reset() {
    kernel::register_job(kernel::kJobUsbFrame, frame);
    g_trace = std::getenv("PS2_TRACE_USB") != nullptr;
    g.on = camera::has_source() ? 1u : 0u;
    controller_reset();
    if (g.on) std::fprintf(stderr, "[usb] camera plugged into USB port 1\n");
}

void soft_reset() { controller_reset(); }

bool on() { return g.on != 0; }

std::uint32_t read(std::uint32_t a) {
    const std::uint32_t reg = (a - 0x1F801600u) >> 2;
    if (reg >= kRegCount) return 0;
    switch (reg) {
    case kInterruptDisable: return g.regs[kInterruptEnable];
    case kFmRemaining: {
        const std::uint32_t fi = g.regs[kFmInterval] & 0x3FFFu;
        return fi ? fi - static_cast<std::uint32_t>((kernel::now() % kFrameCycles) * fi / kFrameCycles) : 0;
    }
    default: return g.regs[reg];
    }
}

void write(std::uint32_t a, std::uint32_t v) {
    const std::uint32_t reg = (a - 0x1F801600u) >> 2;
    if (reg >= kRegCount) return;
    if (g_trace && reg != kInterruptStatus) std::fprintf(stderr, "[usb] write reg %u = 0x%08X\n", reg, v);
    switch (reg) {
    case kRevision: case kPeriodCurrentEd: case kDoneHead: case kFmRemaining: case kFmNumber: case kRhDescriptorA:
        break;
    case kControl:
        g.regs[kControl] = v;
        // a port that changed while the controller was held in reset reports it once the controller runs (RootHubStatusChange)
        if (operational() && ((g.regs[kRhPort0] | g.regs[kRhPort1]) & 0x001F0000u)) set_status(kRhsc);
        schedule_frame();
        break;
    case kCommandStatus:
        if (v & 1u) {
            controller_reset();  // HostControllerReset: back to UsbSuspend with defaults, at once
            return;
        }
        g.regs[kCommandStatus] |= v & 0x0000000Eu;  // CLF / BLF / OCR (the lists are walked every frame anyway)
        break;
    case kInterruptStatus:
        g.regs[kInterruptStatus] &= ~v;
        update_interrupt();
        break;
    case kInterruptEnable:
        g.regs[kInterruptEnable] |= v;
        update_interrupt();
        break;
    case kInterruptDisable:
        g.regs[kInterruptEnable] &= ~v;
        break;
    case kRhStatus:
        break;
    case kRhPort0: case kRhPort1: {
        std::uint32_t &port = g.regs[reg];
        port &= ~(v & 0x001F0000u);  // change bits clear by writing 1
        const bool connected = port & kCcs;
        if (v & 1u) port &= ~kPes;                              // ClearPortEnable
        if ((v & 2u) && connected) port |= kPes;                // SetPortEnable
        if ((v & 0x10u) && connected) {                         // SetPortReset: done at once
            port = (port & ~kPrs) | kPes | kPrsc;
            if (reg == kRhPort0) eyetoy::reset();
            set_status(kRhsc);
        }
        if (v & 0x100u) port |= kPps;
        (void)kPesc;
        break;
    }
    default:
        g.regs[reg] = v;
        break;
    }
}

void after_load(const std::uint32_t *stub, bool had_usb) {
    if (!had_usb) g.on = 0;  // the state is from before this controller: its IOP saw the empty stub
    const bool want = camera::has_source();
    if (want && !g.on) {
        // plug the camera in now: this controller takes over where the stub's registers were (usbd set them up) and reports a
        // new device on port 1, which usbd enumerates like any device plugged in while the game runs
        const std::uint32_t generation = g.generation + 1;
        g = State{};
        g.on = 1;
        g.generation = generation;
        for (unsigned r = kControl; r <= kLsThreshold; ++r) g.regs[r] = stub[r];
        g.regs[kRevision] = 0x110u;
        g.regs[kCommandStatus] &= ~1u;
        g.regs[kFmNumber] = 0;
        if (!g.regs[kFmInterval]) g.regs[kFmInterval] = 0x27782EDFu;
        g.regs[kRhDescriptorA] = 0x00000202u;
        g.regs[kRhPort0] = kPps | kCcs | kCsc;
        g.regs[kRhPort1] = kPps;
        eyetoy::reset();
        if (operational()) set_status(kRhsc);
        schedule_frame();
        std::fprintf(stderr, "[usb] camera plugged into USB port 1 (after loading a state without it)\n");
    } else if (!want && g.on && (g.regs[kRhPort0] & kCcs)) {
        g.regs[kRhPort0] = (g.regs[kRhPort0] & ~(kCcs | kPes)) | kCsc;  // unplugged: the state had a camera, this run has none
        if (operational()) set_status(kRhsc);
        std::fprintf(stderr, "[usb] camera unplugged (the state had one, this run has none)\n");
    }
}

void state(ps2::state::Stream &s) {
    s.tag("usb");
    s.pod(g);
    eyetoy::state(s);
}

}
