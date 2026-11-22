#include "launcher_os.hpp"

#include "launcher_config.hpp"
#include "ps2/os.hpp"

#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif
#include <SDL.h>
#include <SDL_syswm.h>

namespace {
#ifdef _WIN32
std::wstring wide(const std::string &utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}
std::wstring full(const std::string &path) {
    std::error_code e;
    const std::filesystem::path p = std::filesystem::weakly_canonical(ps2::os::path(path), e);
    std::wstring w = e ? ps2::os::path(path).wstring() : p.wstring();
    for (wchar_t &c : w) c = static_cast<wchar_t>(towlower(c));
    return w;
}
#endif
}  // namespace

bool start_and_wait(const std::string &exe, const std::vector<std::string> &args, const std::string &cwd, int &exit_code, std::string &why) {
#ifdef _WIN32
    std::vector<std::string> all = {exe};
    all.insert(all.end(), args.begin(), args.end());
    std::wstring line = wide(windows_command_line(all));
    const std::wstring app = wide(exe), dir = wide(cwd);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(app.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        why = "Windows could not start " + exe + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    exit_code = static_cast<int>(code);
    return true;
#else
    (void)exe;
    (void)args;
    (void)cwd;
    exit_code = -1;
    why = "starting the game is only written for Windows so far";
    return false;
#endif
}

bool run_with_output(const std::string &exe, const std::vector<std::string> &args, const std::string &cwd, const std::string &path_env,
                     const std::function<void(const std::string &line)> &on_line, const std::atomic<bool> &cancel, int &exit_code, std::string &why) {
#ifdef _WIN32
    // the environment: as it is, with PATH replaced
    std::wstring env;
    if (wchar_t *block = GetEnvironmentStringsW()) {
        for (const wchar_t *p = block; *p; p += wcslen(p) + 1) {
            const std::wstring entry = p;
            if (_wcsnicmp(entry.c_str(), L"PATH=", 5) == 0) continue;
            env += entry;
            env += L'\0';
        }
        FreeEnvironmentStringsW(block);
    }
    env += L"PATH=" + wide(path_env);
    env += L'\0';
    env += L'\0';
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        why = "cannot make a pipe";
        return false;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    // an empty but valid stdin: a launcher started by a double-click has none, and the build's tools (sha256sum) fail on a missing one
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    si.hStdInput = nul;
    std::vector<std::string> all = {exe};
    all.insert(all.end(), args.begin(), args.end());
    std::wstring line = wide(windows_command_line(all));
    const std::wstring app = wide(exe), dir = wide(cwd);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(app.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                        env.data(), dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    CloseHandle(write_end);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        why = "Windows could not start " + exe + " (error " + std::to_string(GetLastError()) + ")";
        CloseHandle(read_end);
        if (job) CloseHandle(job);
        return false;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    std::string pending;
    char buffer[4096];
    bool cancelled = false;
    for (;;) {
        DWORD available = 0;
        if (PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD got = 0;
            if (ReadFile(read_end, buffer, sizeof buffer, &got, nullptr) && got > 0) {
                pending.append(buffer, got);
                for (std::size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
                    std::string text = pending.substr(0, nl);
                    if (!text.empty() && text.back() == '\r') text.pop_back();
                    if (on_line) on_line(text);
                    pending.erase(0, nl + 1);
                }
            }
            continue;
        }
        if (cancel.load()) {
            cancelled = true;
            if (job) TerminateJobObject(job, 1);
            else TerminateProcess(pi.hProcess, 1);
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            DWORD left = 0;
            if (!PeekNamedPipe(read_end, nullptr, 0, nullptr, &left, nullptr) || left == 0) break;
        }
    }
    if (!pending.empty() && on_line) on_line(pending);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(read_end);
    if (job) CloseHandle(job);  // (ends anything the program left running)
    exit_code = static_cast<int>(code);
    if (cancelled) {
        why = "cancelled";
        return false;
    }
    return true;
#else
    (void)exe; (void)args; (void)cwd; (void)path_env; (void)on_line; (void)cancel;
    exit_code = -1;
    why = "only written for Windows so far";
    return false;
#endif
}

std::uint64_t free_bytes(const std::string &dir) {
#ifdef _WIN32
    ULARGE_INTEGER free{};
    std::filesystem::path p = ps2::os::path(dir);
    std::error_code e;
    while (!p.empty() && !std::filesystem::exists(p, e)) p = p.parent_path();  // (the folder may not be made yet)
    return GetDiskFreeSpaceExW(p.wstring().c_str(), &free, nullptr, nullptr) ? free.QuadPart : 0;
#else
    (void)dir;
    return 0;
#endif
}

bool is_junction(const std::string &path) {
#ifdef _WIN32
    const DWORD a = GetFileAttributesW(ps2::os::path(path).wstring().c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    (void)path;
    return false;
#endif
}

bool make_junction(const std::string &link, const std::string &target, std::string &why) {
#ifdef _WIN32
    int code = -1;
    const std::atomic<bool> never{false};
    if (!run_with_output("C:\\Windows\\System32\\cmd.exe", {"/c", "mklink", "/J", link, target}, "C:\\Windows\\System32", "C:\\Windows\\System32", nullptr, never,
                         code, why))
        return false;
    if (code != 0 || !is_junction(link)) {
        why = "cannot link " + link + " to " + target + " (mklink /J: code " + std::to_string(code) + ")";
        return false;
    }
    return true;
#else
    (void)link; (void)target;
    why = "only written for Windows so far";
    return false;
#endif
}

bool remove_junction(const std::string &link) {
#ifdef _WIN32
    if (!is_junction(link)) return false;
    return RemoveDirectoryW(ps2::os::path(link).wstring().c_str()) != 0;  // (removes the link, never what it points to)
#else
    (void)link;
    return false;
#endif
}

bool runner_running(const std::string &exe) {
#ifdef _WIN32
    const std::wstring want = full(exe);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &entry); ok && !found; ok = Process32NextW(snap, &entry)) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!h) continue;
        wchar_t name[MAX_PATH * 2];
        DWORD size = static_cast<DWORD>(std::size(name));
        if (QueryFullProcessImageNameW(h, 0, name, &size)) {
            std::wstring got(name, size);
            for (wchar_t &c : got) c = static_cast<wchar_t>(towlower(c));
            found = got == want;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
#else
    (void)exe;
    return false;
#endif
}

#ifdef _WIN32
namespace {
HWND hwnd_of(SDL_Window *window) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    return SDL_GetWindowWMInfo(window, &info) ? info.info.win.window : nullptr;
}
}  // namespace
#endif

bool make_layered(SDL_Window *window) {
#ifdef _WIN32
    HWND h = hwnd_of(window);
    if (!h) return false;
    SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) | WS_EX_LAYERED);
    return true;
#else
    (void)window;
    return false;
#endif
}

bool present_layered(SDL_Window *window, const std::vector<std::uint32_t> &bgra, int width, int height) {
#ifdef _WIN32
    HWND h = hwnd_of(window);
    if (!h || width <= 0 || height <= 0 || bgra.size() < static_cast<std::size_t>(width) * height) return false;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bitmap && bits) {
        std::memcpy(bits, bgra.data(), static_cast<std::size_t>(width) * height * 4);
        HGDIOBJ old = SelectObject(memory, bitmap);
        SIZE size{width, height};
        POINT from{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        ok = UpdateLayeredWindow(h, screen, nullptr, &size, memory, &from, 0, &blend, ULW_ALPHA) != 0;
        SelectObject(memory, old);
    }
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return ok;
#else
    (void)window;
    (void)bgra;
    (void)width;
    (void)height;
    return false;
#endif
}

std::string launcher_dir() {
#ifdef _WIN32
    wchar_t path[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return ps2::os::utf8(std::filesystem::path(std::wstring(path, n)).parent_path());
#else
    return ".";
#endif
}
