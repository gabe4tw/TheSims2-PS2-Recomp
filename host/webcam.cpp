#include "webcam.hpp"

#include "ps2/camera.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#endif

namespace webcam {

namespace {

std::mutex g_mutex;
ps2::camera::Picture g_picture;  // the newest picture
bool g_have = false;
std::atomic<bool> g_stop{false};
std::thread g_thread;

bool take(ps2::camera::Picture &out) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_have) return false;
    out = g_picture;
    return true;
}

void publish(ps2::camera::Picture &&p) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_picture = std::move(p);
    g_have = true;
}

// "test": colour bars with a ball moving across, so the camera path can be checked without a webcam
void test_thread() {
    int t = 0;
    while (!g_stop) {
        ps2::camera::Picture p;
        p.width = 640;
        p.height = 480;
        p.rgb.resize(640 * 480 * 3);
        const double bx = 320 + 220 * std::sin(t * 0.05), by = 240 + 120 * std::cos(t * 0.07);
        for (int y = 0; y < 480; ++y)
            for (int x = 0; x < 640; ++x) {
                std::uint8_t *o = &p.rgb[(y * 640 + x) * 3];
                static const std::uint8_t bars[8][3] = {{235, 235, 235}, {235, 235, 16}, {16, 235, 235}, {16, 235, 16},
                                                        {235, 16, 235}, {235, 16, 16},   {16, 16, 235},  {16, 16, 16}};
                const std::uint8_t *c = bars[x / 80];
                o[0] = c[0];
                o[1] = c[1];
                o[2] = c[2];
                if (y > 400) o[0] = o[1] = o[2] = static_cast<std::uint8_t>(x * 255 / 639);
                if ((x - bx) * (x - bx) + (y - by) * (y - by) < 50 * 50) {
                    o[0] = 255;
                    o[1] = 140;
                    o[2] = 0;
                }
            }
        publish(std::move(p));
        ++t;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}

#ifdef _WIN32
template <typename T>
void release(T *&p) {
    if (p) p->Release();
    p = nullptr;
}

std::string narrow(const wchar_t *w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

struct Devices {
    IMFActivate **items = nullptr;
    UINT32 count = 0;
    Devices() {
        IMFAttributes *attr = nullptr;
        if (FAILED(MFCreateAttributes(&attr, 1))) return;
        attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (FAILED(MFEnumDeviceSources(attr, &items, &count))) {
            items = nullptr;
            count = 0;
        }
        attr->Release();
    }
    ~Devices() {
        for (UINT32 i = 0; i < count; ++i) items[i]->Release();
        CoTaskMemFree(items);
    }
    std::string name(UINT32 i) const {
        WCHAR *w = nullptr;
        UINT32 len = 0;
        std::string s;
        if (SUCCEEDED(items[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &w, &len))) s = narrow(w);
        CoTaskMemFree(w);
        return s;
    }
};

struct MfSession {
    bool ok = false, com = false;
    MfSession() {
        // a thread SDL already set up for COM in the other mode keeps it (RPC_E_CHANGED_MODE): Media Foundation works in either
        com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }
    ~MfSession() {
        if (ok) MFShutdown();
        if (com) CoUninitialize();
    }
};

// The capture: opens the device by its index, asks the source reader for RGB32 (it converts from the camera's own format), picks the
// camera format nearest 640 x 480 and keeps reading until stop().
void capture_thread(std::string wanted) {
    MfSession mf;
    if (!mf.ok) return;
    IMFMediaSource *source = nullptr;
    IMFSourceReader *reader = nullptr;
    {
        Devices d;
        for (UINT32 i = 0; i < d.count && !source; ++i) {
            const std::string name = d.name(i);
            if (wanted == "auto" || name == wanted || name.find(wanted) != std::string::npos) {
                if (SUCCEEDED(d.items[i]->ActivateObject(IID_PPV_ARGS(&source))))
                    std::fprintf(stderr, "[webcam] opened \"%s\"\n", name.c_str());
                else
                    std::fprintf(stderr, "[webcam] cannot open \"%s\"\n", name.c_str());
            }
        }
    }
    if (!source) {
        std::fprintf(stderr, "[webcam] no webcam \"%s\": the EyeToy shows no picture\n", wanted.c_str());
        return;
    }
    IMFAttributes *attr = nullptr;
    MFCreateAttributes(&attr, 2);
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    HRESULT hr = MFCreateSourceReaderFromMediaSource(source, attr, &reader);
    release(attr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[webcam] no source reader (0x%08lX)\n", static_cast<unsigned long>(hr));
        source->Shutdown();
        release(source);
        return;
    }
    const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    // the camera's own format closest to 640 x 480 (more pixels are only shrunk away; a 4:3 one avoids cropping)
    {
        IMFMediaType *best = nullptr;
        long long best_score = -1;
        for (DWORD i = 0;; ++i) {
            IMFMediaType *t = nullptr;
            if (FAILED(reader->GetNativeMediaType(stream, i, &t))) break;
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
            const long long score = 1000000 - std::llabs(static_cast<long long>(w) * h - 640LL * 480) / 16 - (w * 3 == h * 4 ? 0 : 5000);
            if (w >= 320 && score > best_score) {
                release(best);
                best = t;
                best_score = score;
            } else {
                t->Release();
            }
        }
        if (best) {
            reader->SetCurrentMediaType(stream, nullptr, best);
            release(best);
        }
    }
    IMFMediaType *rgb = nullptr;
    MFCreateMediaType(&rgb);
    rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = reader->SetCurrentMediaType(stream, nullptr, rgb);
    release(rgb);
    if (FAILED(hr)) std::fprintf(stderr, "[webcam] cannot get RGB pictures (0x%08lX)\n", static_cast<unsigned long>(hr));
    UINT32 w = 0, h = 0;
    LONG stride = 0;  // bytes from one row to the next in a plain buffer; negative: the rows are stored bottom-up
    {
        IMFMediaType *cur = nullptr;
        if (SUCCEEDED(reader->GetCurrentMediaType(stream, &cur))) {
            MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
            UINT32 given = 0;
            if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &given))) stride = static_cast<LONG>(static_cast<INT32>(given));
            else if (FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, w, &stride))) stride = static_cast<LONG>(w * 4);
            cur->Release();
        }
    }
    std::fprintf(stderr, "[webcam] capturing %ux%u\n", w, h);
    while (!g_stop && SUCCEEDED(hr) && w && h) {
        DWORD actual = 0, flags = 0;
        LONGLONG when = 0;
        IMFSample *sample = nullptr;
        hr = reader->ReadSample(stream, 0, &actual, &flags, &when, &sample);
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
            std::fprintf(stderr, "[webcam] the camera stopped (0x%08lX, flags 0x%lX)\n", static_cast<unsigned long>(hr), static_cast<unsigned long>(flags));
            release(sample);
            break;
        }
        if (!sample) continue;
        IMFMediaBuffer *buffer = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
            ps2::camera::Picture p;
            p.width = static_cast<int>(w);
            p.height = static_cast<int>(h);
            p.rgb.resize(static_cast<std::size_t>(w) * h * 3);
            IMF2DBuffer *two_d = nullptr;
            BYTE *base = nullptr;
            LONG pitch = 0;
            DWORD max_len = 0, len = 0;
            bool locked = false;
            if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&two_d))) && SUCCEEDED(two_d->Lock2D(&base, &pitch))) {
                locked = true;
            } else if (SUCCEEDED(buffer->Lock(&base, &max_len, &len))) {
                pitch = stride ? stride : static_cast<LONG>(w * 4);
                if (pitch < 0) base += static_cast<std::size_t>(-pitch) * (h - 1);  // bottom-up: the top row is the last one
                locked = len >= w * h * 4;
                if (!locked) buffer->Unlock();
            }
            if (locked) {
                for (UINT32 y = 0; y < h; ++y) {
                    const BYTE *row = base + static_cast<std::ptrdiff_t>(pitch) * y;
                    std::uint8_t *o = &p.rgb[static_cast<std::size_t>(y) * w * 3];
                    for (UINT32 x = 0; x < w; ++x) {
                        o[x * 3] = row[x * 4 + 2];
                        o[x * 3 + 1] = row[x * 4 + 1];
                        o[x * 3 + 2] = row[x * 4];
                    }
                }
                if (two_d) two_d->Unlock2D();
                else buffer->Unlock();
                publish(std::move(p));
            }
            release(two_d);
            buffer->Release();
        }
        sample->Release();
    }
    release(reader);
    source->Shutdown();
    release(source);
}
#endif

}

std::vector<std::string> list() {
    std::vector<std::string> names;
#ifdef _WIN32
    MfSession mf;
    if (!mf.ok) return names;
    Devices d;
    for (UINT32 i = 0; i < d.count; ++i) names.push_back(d.name(i));
#endif
    return names;
}

std::string start(const std::string &choice) {
    if (choice.empty() || choice == "none" || choice == "0" || choice == "off") return {};
    g_stop = false;
    if (choice == "test") {
        g_thread = std::thread(test_thread);
        g_thread.detach();
    } else {
        // only a webcam that is there now is plugged in: with none, the game sees an empty USB port as before
        std::string found;
        for (const std::string &name : list())
            if (choice == "auto" || name == choice || name.find(choice) != std::string::npos) {
                found = name;
                break;
            }
        if (found.empty()) {
            std::fprintf(stderr, "[webcam] no webcam \"%s\" on this PC: no EyeToy plugged in\n", choice.c_str());
            return {};
        }
#ifdef _WIN32
        g_thread = std::thread(capture_thread, found);
        g_thread.detach();  // it ends with the process (a camera can block in ReadSample)
#else
        return {};
#endif
        ps2::camera::set_source(take);
        return found;
    }
    ps2::camera::set_source(take);
    return choice;
}

void stop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.detach();  // a camera can block in ReadSample; the process is ending anyway
}

}
