// The virtual EyeToy's picture encoder against the runtime's IPU, no game needed: ninja ps2eyetoytest, then
// ps2eyetoytest [out.ppm]. Encodes a test picture the way the camera sends it, decodes it the way libeyety does (IPU_CTRL from the
// first byte, FDEC 8 past it, IDEC with quantiser 1 to RGB16) and compares.
#include "ipu/ipu.hpp"
#include "iop/eyetoy.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char **argv) {
    const int w = 320, h = 240;
    std::vector<std::uint8_t> rgb(w * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::uint8_t *p = &rgb[(y * w + x) * 3];
            const double cx = x - 160.0, cy = y - 120.0;
            p[0] = static_cast<std::uint8_t>(x * 255 / w);
            p[1] = static_cast<std::uint8_t>(y * 255 / h);
            p[2] = (std::hypot(cx, cy) < 60) ? 230 : ((x / 20 + y / 20) & 1 ? 40 : 200);
        }
    int failures = 0;
    for (int q : {2, 4, 8, 16}) {
        const std::vector<std::uint8_t> frame = ps2::iop::eyetoy::encode(rgb.data(), w, h, q);
        if (std::memcmp(frame.data(), "ipum", 4) != 0 || frame.size() % 16) {
            std::printf("FAIL q %d: bad header\n", q);
            ++failures;
            continue;
        }
        ps2::ipu::reset();
        ps2::ipu::write32(0x10002010u, 0x40000000u);  // reset
        ps2::ipu::write32(0x10002010u, static_cast<std::uint32_t>(frame[16] & 0xFB) << 16);
        for (std::size_t i = 16; i < frame.size(); i += 16) ps2::ipu::write_fifo(frame.data() + i);
        ps2::ipu::write32(0x10002000u, 0x40000008u);  // FDEC past the control byte
        ps2::ipu::write32(0x10002000u, 0x1C010000u);  // IDEC, quantiser 1, dither, RGB16
        const int mbs = (w / 16) * (h / 16);
        std::vector<std::uint8_t> out(static_cast<std::size_t>(mbs) * 512);
        for (std::size_t i = 0; i < out.size(); i += 16) ps2::ipu::read_fifo(out.data() + i);
        double err = 0;
        for (int mb = 0; mb < mbs; ++mb)
            for (int py = 0; py < 16; ++py)
                for (int px = 0; px < 16; ++px) {
                    const int x = (mb % (w / 16)) * 16 + px, y = (mb / (w / 16)) * 16 + py;
                    std::uint16_t v;
                    std::memcpy(&v, &out[mb * 512 + (py * 16 + px) * 2], 2);
                    const int r = (v & 31) << 3, g = ((v >> 5) & 31) << 3, b = ((v >> 10) & 31) << 3;
                    const std::uint8_t *s = &rgb[(y * w + x) * 3];
                    err += (r - s[0]) * (r - s[0]) + (g - s[1]) * (g - s[1]) + (b - s[2]) * (b - s[2]);
                    if (argc > 1 && q == 4) {
                        static std::vector<std::uint8_t> img(w * h * 3);
                        img[(y * w + x) * 3] = static_cast<std::uint8_t>(r);
                        img[(y * w + x) * 3 + 1] = static_cast<std::uint8_t>(g);
                        img[(y * w + x) * 3 + 2] = static_cast<std::uint8_t>(b);
                        if (mb == mbs - 1 && py == 15 && px == 15) {
                            FILE *f = std::fopen(argv[1], "wb");
                            std::fprintf(f, "P6 %d %d 255\n", w, h);
                            std::fwrite(img.data(), 1, img.size(), f);
                            std::fclose(f);
                        }
                    }
                }
        const double psnr = 10 * std::log10(255.0 * 255.0 / (err / (w * h * 3)));
        const bool ok = psnr > 24;
        failures += !ok;
        std::printf("%s q %2d: %zu bytes, PSNR %.1f dB (RGB16)\n", ok ? "ok  " : "FAIL", q, frame.size(), psnr);
    }
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::size_t bytes = 0;
        for (int i = 0; i < 100; ++i) bytes += ps2::iop::eyetoy::encode(rgb.data(), w, h, 2).size();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 100;
        std::printf("encode 320x240: %.2f ms per frame (%zu bytes)\n", ms, bytes / 100);
    }
    return failures ? 1 : 0;
}
