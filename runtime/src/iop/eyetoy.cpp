#include "eyetoy.hpp"

#include "../ipu/tables.hpp"

#include "ps2/camera.hpp"
#include "ps2/state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace ps2::camera {

namespace {
std::mutex g_mutex;
Source g_source;
}

void set_source(Source source) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_source = std::move(source);
}

bool has_source() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<bool>(g_source);
}

bool latest(Picture &out) {
    Source s;
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        s = g_source;
    }
    return s && s(out);
}

}

namespace ps2::iop::eyetoy {

namespace {

// ---- descriptors -----------------------------------------------------------------------------------------------------------------

constexpr std::uint8_t kDevice[18] = {18, 1, 0x10, 0x01, 0, 0, 0, 8, 0x4C, 0x05, 0x55, 0x01, 0x00, 0x01, 1, 2, 0, 1};
constexpr std::uint16_t kAltSizes[5] = {0, 384, 512, 768, 896};

std::vector<std::uint8_t> config_descriptor() {
    std::vector<std::uint8_t> d = {9, 2, 0, 0, 3, 1, 0, 0x80, 250};
    for (int alt = 0; alt < 5; ++alt) {
        const std::uint8_t itf[9] = {9, 4, 0, static_cast<std::uint8_t>(alt), 1, 0xFF, 0, 0, 0};
        const std::uint8_t ep[7] = {7, 5, 0x81, 1, static_cast<std::uint8_t>(kAltSizes[alt] & 0xFF), static_cast<std::uint8_t>(kAltSizes[alt] >> 8), 1};
        d.insert(d.end(), itf, itf + 9);
        d.insert(d.end(), ep, ep + 7);
    }
    // The microphone (USB audio class 1.0). usbcam tells the EE that a camera is there (SIF software register 7) only once it has
    // set this up too: it wants an AudioControl interface whose second descriptor on is a microphone input terminal (type 0x0201),
    // and an AudioStreaming alternate with a 16-bit format offering 16000 Hz, then an isochronous IN endpoint.
    static const std::uint8_t audio[] = {
        9, 4, 1, 0, 0, 1, 1, 0, 0,                    // interface 1: AudioControl
        9, 0x24, 1, 0x00, 0x01, 30, 0, 1, 2,          // header: ADC 1.00, 30 bytes, streaming interface 2
        12, 0x24, 2, 1, 0x01, 0x02, 0, 1, 0, 0, 0, 0, // input terminal 1: microphone, mono
        9, 0x24, 3, 2, 0x01, 0x01, 0, 1, 0,           // output terminal 2: USB streaming, from terminal 1
        9, 4, 2, 0, 0, 1, 2, 0, 0,                    // interface 2 alternate 0: AudioStreaming, no endpoint
        9, 4, 2, 1, 1, 1, 2, 0, 0,                    // interface 2 alternate 1: one endpoint
        7, 0x24, 1, 2, 1, 1, 0,                       // general: terminal 2, PCM
        11, 0x24, 2, 1, 1, 2, 16, 1, 0x80, 0x3E, 0,   // format type I: mono, 2 bytes, 16 bits, 16000 Hz
        9, 5, 0x82, 5, 32, 0, 1, 0, 0,                // endpoint 2 IN, isochronous asynchronous, 32 bytes
        7, 0x25, 1, 1, 0, 0, 0,                       // class endpoint: sampling frequency control
    };
    d.insert(d.end(), audio, audio + sizeof audio);
    d[2] = static_cast<std::uint8_t>(d.size());
    d[3] = static_cast<std::uint8_t>(d.size() >> 8);
    return d;
}

std::vector<std::uint8_t> string_descriptor(int index) {
    if (index == 0) return {4, 3, 0x09, 0x04};
    const char *text = index == 1 ? "Sony" : "EyeToy USB camera Namtai";
    std::vector<std::uint8_t> d = {0, 3};
    for (const char *p = text; *p; ++p) {
        d.push_back(static_cast<std::uint8_t>(*p));
        d.push_back(0);
    }
    d[0] = static_cast<std::uint8_t>(d.size());
    return d;
}

// ---- device state (kept in save states) ------------------------------------------------------------------------------------------

struct Regs {
    std::uint8_t address = 0, config = 0, alt = 0, pending_address = 0;
    std::uint8_t has_pending_address = 0, out_request = 0, frame_sent = 0, pad1 = 0;  // frame_sent: the EOF packet went out
    std::uint8_t setup[8] = {};
    std::uint8_t bridge[256] = {};
    std::uint8_t sensor[256] = {};
    std::uint32_t reply_pos = 0;
    std::uint32_t packet = 0;     // next packet of the frame being sent
    std::uint32_t idle = 0;       // USB frames since the current camera frame started
    std::uint32_t qscale = 4;
    std::uint32_t frames = 0;
};

Regs g;
std::vector<std::uint8_t> g_reply, g_out;
std::vector<std::uint8_t> g_frame;  // the camera frame on its way, as sent: SOF header + data, then the EOF header
camera::Picture g_last;             // the last picture the source gave (not in save states)
bool g_trace = false;

void reset_sensor() {
    std::memset(g.sensor, 0, sizeof g.sensor);
    g.sensor[0x0A] = 0x76;  // product id: OV7648
    g.sensor[0x0B] = 0x48;
    g.sensor[0x1C] = 0x7F;  // manufacturer: OmniVision
    g.sensor[0x1D] = 0xA2;
}

void write_bridge(std::uint8_t reg, std::uint8_t v) {
    g.bridge[reg] = v;
    if (reg != 0x47) return;
    switch (v) {
    case 1: g.sensor[g.bridge[0x42]] = g.bridge[0x45]; break;           // I2C write
    case 5: case 3: g.bridge[0x45] = g.sensor[g.bridge[0x43]]; break;  // I2C read
    default: break;
    }
}

// ---- the picture encoder: MPEG intra macroblocks as the IPU's IDEC reads them ----------------------------------------------------

class Bits {
public:
    void put(std::uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            acc_ = static_cast<std::uint8_t>((acc_ << 1) | ((value >> i) & 1u));
            if (++n_ == 8) {
                out.push_back(acc_);
                acc_ = 0;
                n_ = 0;
            }
        }
    }
    void align() {
        while (n_) put(0, 1);
    }
    std::vector<std::uint8_t> out;

private:
    std::uint8_t acc_ = 0;
    int n_ = 0;
};

struct Code {
    std::uint16_t code = 0;
    std::uint8_t length = 0;
};

struct EncodeTables {
    Code ac[32][41];  // run, level (table B-14)
    Code dc_luma[12], dc_chroma[12];
    float basis[8][8];
    EncodeTables() {
        for (const auto &e : ipu::tables::kDctTable0)
            if (e.run < 32 && e.level > 0 && e.level <= 40) ac[e.run][e.level] = {e.code, e.length};  // run 0 level 1 is "11" here (not the first-coefficient "1")
        for (const auto &e : ipu::tables::kDcSizeLuma) dc_luma[e.value] = {e.code, e.length};
        for (const auto &e : ipu::tables::kDcSizeChroma) dc_chroma[e.value] = {e.code, e.length};
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x)
                basis[u][x] = static_cast<float>(0.5 * (u ? 1.0 : std::sqrt(0.5)) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0));
    }
};

const EncodeTables &enc() {
    static const EncodeTables t;
    return t;
}

constexpr std::uint8_t kIntraMatrix[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
};

void put_dc(Bits &b, int diff, bool chroma) {
    const int mag = std::abs(diff);
    int size = 0;
    while ((1 << size) <= mag) ++size;
    const Code &c = chroma ? enc().dc_chroma[size] : enc().dc_luma[size];
    b.put(c.code, c.length);
    if (size) b.put(static_cast<std::uint32_t>(diff > 0 ? diff : diff + (1 << size) - 1), size);
}

void put_block(Bits &b, const float *px, int &dc_pred, bool chroma, int qscale) {
    const auto &t = enc();
    float tmp[64], f[64];
    for (int y = 0; y < 8; ++y)
        for (int u = 0; u < 8; ++u) {
            float s = 0;
            for (int x = 0; x < 8; ++x) s += px[y * 8 + x] * t.basis[u][x];
            tmp[y * 8 + u] = s;
        }
    for (int v = 0; v < 8; ++v)
        for (int u = 0; u < 8; ++u) {
            float s = 0;
            for (int y = 0; y < 8; ++y) s += tmp[y * 8 + u] * t.basis[v][y];
            f[v * 8 + u] = s;
        }
    const int dc = std::clamp(static_cast<int>(std::lround(f[0] / 8.0f)), 1, 254);
    put_dc(b, dc - dc_pred, chroma);
    dc_pred = dc;
    int run = 0;
    for (int i = 1; i < 64; ++i) {
        const int pos = ipu::tables::kZigzag[i];
        const float q = f[pos] * 8.0f / static_cast<float>(kIntraMatrix[pos] * qscale);
        const int level = std::clamp(static_cast<int>(std::lround(q)), -2047, 2047);
        if (!level) {
            ++run;
            continue;
        }
        const int mag = std::abs(level);
        if (mag <= 40 && t.ac[run][mag].length) {
            b.put(t.ac[run][mag].code, t.ac[run][mag].length);
            b.put(level < 0 ? 1u : 0u, 1);
        } else {
            b.put(1, 6);  // escape, MPEG-2 form: 6-bit run, 12-bit level
            b.put(static_cast<std::uint32_t>(run), 6);
            b.put(static_cast<std::uint32_t>(level) & 0xFFFu, 12);
        }
        run = 0;
    }
    b.put(2, 2);  // end of block
}

}

std::vector<std::uint8_t> encode(const std::uint8_t *rgb, int w, int h, int qscale) {
    qscale = std::clamp(qscale, 1, 31);
    const int mbw = (w + 15) / 16, mbh = (h + 15) / 16;
    Bits b;
    b.put(0, 8);  // the IPU_CTRL byte: 8-bit DC, zigzag, table B-14, linear quantiser, MPEG-2 rules, no dct_type
    int pred[3] = {128, 128, 128};
    for (int my = 0; my < mbh; ++my)
        for (int mx = 0; mx < mbw; ++mx) {
            if (my || mx) b.put(1, 1);  // macroblock_address_increment 1
            if (!my && !mx) {
                b.put(1, 2);  // intra with quantiser_scale_code
                b.put(static_cast<std::uint32_t>(qscale), 5);
            } else {
                b.put(1, 1);  // intra
            }
            float y[4][64], cb[64] = {}, cr[64] = {};
            for (int py = 0; py < 16; ++py)
                for (int px = 0; px < 16; ++px) {
                    const int sx = std::min(mx * 16 + px, w - 1), sy = std::min(my * 16 + py, h - 1);
                    const std::uint8_t *p = rgb + (static_cast<std::size_t>(sy) * w + sx) * 3;
                    const float r = p[0], g = p[1], bl = p[2];
                    y[(py / 8) * 2 + px / 8][(py & 7) * 8 + (px & 7)] = 16.0f + (65.738f * r + 129.057f * g + 25.064f * bl) / 256.0f;
                    const int ci = (py / 2) * 8 + px / 2;
                    cb[ci] += 0.25f * (128.0f + (-37.945f * r - 74.494f * g + 112.439f * bl) / 256.0f);
                    cr[ci] += 0.25f * (128.0f + (112.439f * r - 94.154f * g - 18.285f * bl) / 256.0f);
                }
            for (int k = 0; k < 4; ++k) put_block(b, y[k], pred[0], false, qscale);
            put_block(b, cb, pred[1], true, qscale);
            put_block(b, cr, pred[2], true, qscale);
        }
    b.align();
    b.put(0x000001B7u, 32);  // the start code that ends IDEC
    std::vector<std::uint8_t> frame(16, 0);
    std::memcpy(frame.data(), "ipum", 4);
    frame.insert(frame.end(), b.out.begin(), b.out.end());
    frame.resize(((frame.size() + 15) & ~std::size_t{15}) + 16, 0);
    const std::uint32_t total = static_cast<std::uint32_t>(frame.size());
    std::memcpy(frame.data() + 4, &total, 4);
    const std::uint16_t ww = static_cast<std::uint16_t>(w), hh = static_cast<std::uint16_t>(h);
    std::memcpy(frame.data() + 8, &ww, 2);
    std::memcpy(frame.data() + 10, &hh, 2);
    return frame;
}

namespace {

// The newest camera picture cropped to the frame's shape and scaled to w x h (box filter).
std::vector<std::uint8_t> picture_for(int w, int h) {
    camera::Picture pic;
    if (camera::latest(pic) && pic.width > 0 && pic.height > 0 && pic.rgb.size() >= static_cast<std::size_t>(pic.width) * pic.height * 3)
        g_last = std::move(pic);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 3, 0x80);
    if (!g_last.width) return out;
    const int sw = g_last.width, sh = g_last.height;
    int cw = sw, ch = sh, cx = 0, cy = 0;
    if (static_cast<long long>(sw) * h > static_cast<long long>(sh) * w) {
        cw = static_cast<int>(static_cast<long long>(sh) * w / h);
        cx = (sw - cw) / 2;
    } else {
        ch = static_cast<int>(static_cast<long long>(sw) * h / w);
        cy = (sh - ch) / 2;
    }
    for (int y = 0; y < h; ++y) {
        const int y0 = cy + y * ch / h, y1 = std::max(y0 + 1, cy + (y + 1) * ch / h);
        for (int x = 0; x < w; ++x) {
            const int x0 = cx + x * cw / w, x1 = std::max(x0 + 1, cx + (x + 1) * cw / w);
            unsigned sum[3] = {}, n = 0;
            for (int yy = y0; yy < y1; yy += std::max(1, (y1 - y0) / 4))
                for (int xx = x0; xx < x1; xx += std::max(1, (x1 - x0) / 4)) {
                    const std::uint8_t *p = g_last.rgb.data() + (static_cast<std::size_t>(yy) * sw + xx) * 3;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    ++n;
                }
            std::uint8_t *o = out.data() + (static_cast<std::size_t>(y) * w + x) * 3;
            for (int k = 0; k < 3; ++k) o[k] = static_cast<std::uint8_t>(sum[k] / n);
        }
    }
    return out;
}

constexpr std::size_t kFrameLimit = 20480;  // usbcam's "frame size" (the module's framesize= argument)

void start_frame() {
    int w = g.bridge[0x10] * 16, h = g.bridge[0x11] * 8;
    if (w <= 0 || h <= 0 || w > 640 || h > 480) {
        w = 320;
        h = 240;
    }
    const std::vector<std::uint8_t> rgb = picture_for(w, h);
    std::vector<std::uint8_t> body;
    for (;;) {
        body = encode(rgb.data(), w, h, static_cast<int>(g.qscale));
        if (body.size() + 16 <= kFrameLimit - 64 || g.qscale >= 31) break;
        g.qscale = std::min<std::uint32_t>(31, g.qscale + std::max<std::uint32_t>(1, g.qscale / 3));
    }
    if (body.size() + 16 < (kFrameLimit * 5) / 8 && g.qscale > 1) --g.qscale;
    g_frame.assign(16, 0);
    g_frame[0] = g_frame[1] = g_frame[2] = 0xFF;
    g_frame[3] = 0x50;
    g_frame.insert(g_frame.end(), body.begin(), body.end());
    const std::size_t eof = g_frame.size();
    g_frame.resize(eof + 16, 0);
    g_frame[eof] = g_frame[eof + 1] = g_frame[eof + 2] = 0xFF;
    g_frame[eof + 3] = 0x51;
    const std::uint16_t eights = static_cast<std::uint16_t>(body.size() / 8);
    g_frame[eof + 10] = static_cast<std::uint8_t>(eights);
    g_frame[eof + 11] = static_cast<std::uint8_t>(eights >> 8);
    g.packet = 0;
    g.frame_sent = 0;
    ++g.frames;
    if (g_trace && g.frames % 30 == 1)
        std::fprintf(stderr, "[eyetoy] frame %u: %dx%d, %zu bytes, quantiser %u\n", g.frames, w, h, body.size(), g.qscale);
}

}

void reset() {
    g = Regs{};
    reset_sensor();
    g_reply.clear();
    g_out.clear();
    g_frame.clear();
    g_trace = std::getenv("PS2_TRACE_USB") != nullptr;
}

std::uint8_t address() { return g.address; }

bool setup(const std::uint8_t p[8]) {
    std::memcpy(g.setup, p, 8);
    g_reply.clear();
    g_out.clear();
    g.reply_pos = 0;
    g.out_request = 0;
    const std::uint8_t type = p[0], request = p[1];
    const unsigned value = p[2] | (p[3] << 8), index = p[4] | (p[5] << 8), length = p[6] | (p[7] << 8);
    if (g_trace && !(type & 0x60))
        std::fprintf(stderr, "[eyetoy] setup %02X %02X value %04X index %04X length %u\n", type, request, value, index, length);
    if ((type & 0x60) == 0) {  // standard
        switch (request) {
        case 0:  // GET_STATUS
            g_reply = {0, 0};
            break;
        case 1: case 3:  // CLEAR_FEATURE, SET_FEATURE
            break;
        case 5:  // SET_ADDRESS, done at the status stage
            g.pending_address = static_cast<std::uint8_t>(value & 0x7F);
            g.has_pending_address = 1;
            break;
        case 6:  // GET_DESCRIPTOR
            switch (value >> 8) {
            case 1: g_reply.assign(kDevice, kDevice + 18); break;
            case 2: g_reply = config_descriptor(); break;
            case 3:
                if ((value & 0xFF) > 2) return false;
                g_reply = string_descriptor(value & 0xFF);
                break;
            default: return false;
            }
            break;
        case 8: g_reply = {g.config}; break;
        case 9: g.config = static_cast<std::uint8_t>(value); break;
        case 10: g_reply = {g.alt}; break;
        case 11:
            if (index == 0) g.alt = static_cast<std::uint8_t>(std::min(value, 4u));
            if (g_trace) std::fprintf(stderr, "[eyetoy] interface %u alternate %u\n", index, value);
            break;
        default: return false;
        }
    } else if ((type & 0x60) == 0x40 && request == 1) {  // the bridge's registers
        if (type & 0x80) {
            g_reply = {g.bridge[index & 0xFF]};
            if (g_trace) std::fprintf(stderr, "[eyetoy] read  reg %02X = %02X\n", index & 0xFF, g.bridge[index & 0xFF]);
        } else {
            g.out_request = 1;
        }
    } else if (type & 0x80) {
        g_reply.assign(length, 0);
    }
    if (g_reply.size() > length) g_reply.resize(length);
    return true;
}

int in(std::uint8_t *dst, int max) {
    const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(std::max(max, 0)), g_reply.size() - std::min<std::size_t>(g.reply_pos, g_reply.size())));
    if (n > 0) std::memcpy(dst, g_reply.data() + g.reply_pos, static_cast<std::size_t>(n));
    g.reply_pos += static_cast<std::uint32_t>(std::max(n, 0));
    return n;
}

int out(const std::uint8_t *src, int n) {
    g_out.insert(g_out.end(), src, src + n);
    return n;
}

void status() {
    if (g.has_pending_address) {
        g.address = g.pending_address;
        g.has_pending_address = 0;
        if (g_trace) std::fprintf(stderr, "[eyetoy] address %u\n", g.address);
    }
    if (g.out_request && !g_out.empty()) {
        const unsigned index = g.setup[4] | (g.setup[5] << 8);
        if (g_trace) std::fprintf(stderr, "[eyetoy] write reg %02X = %02X\n", index & 0xFF, g_out[0]);
        write_bridge(static_cast<std::uint8_t>(index), g_out[0]);
    }
    g.out_request = 0;
    g_out.clear();
}

int iso_in(int endpoint, std::uint8_t *dst, int max) {
    if (endpoint == 2) {  // the microphone: silence
        const int n = std::min(max, 32);
        std::memset(dst, 0, static_cast<std::size_t>(std::max(n, 0)));
        return n;
    }
    if (endpoint != 1 || g.alt == 0) return -1;
    const int mps = std::min<int>(max, kAltSizes[g.alt]) & ~15;
    ++g.idle;
    if (g_frame.empty() || g.frame_sent) {
        // the frame was sent (its last packet is the EOF header alone); start the next one about every 33 ms
        if (!g_frame.empty() && g.idle < 33) return 0;
        g.idle = 0;
        start_frame();
    }
    // packets: [SOF header + data] [data] ... [EOF header]; the data part is everything before the EOF header
    const std::size_t data_end = g_frame.size() - 16;
    const std::size_t from = static_cast<std::size_t>(g.packet) * static_cast<std::size_t>(mps);
    ++g.packet;
    if (from >= data_end) {
        std::memcpy(dst, g_frame.data() + data_end, 16);
        g.frame_sent = 1;  // done: a new frame starts once its time has come
        return 16;
    }
    const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(mps), data_end - from));
    std::memcpy(dst, g_frame.data() + from, static_cast<std::size_t>(n));
    return n;
}

void state(ps2::state::Stream &s) {
    s.tag("eyetoy");
    s.pod(g);
    s.vec(g_reply);
    s.vec(g_out);
    s.vec(g_frame);
}

}
