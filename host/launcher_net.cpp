#include "launcher_net.hpp"

#include "ps2/os.hpp"

#include <cstdio>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#endif

namespace {
#ifdef _WIN32
std::wstring wide(const std::string &utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET x) : h(x) {}
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};

// one request; `sink` gets the body in pieces (returning false stops)
bool get(const std::string &url, const std::function<bool(const char *, std::size_t, std::uint64_t total)> &sink, std::string &why) {
    std::wstring w = wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256] = {}, path[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    wchar_t extra[2048] = {};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &parts)) {
        why = "not a web address: " + url;
        return false;
    }
    const std::string host_text = ps2::os::utf8(std::filesystem::path(std::wstring(host)));
    Handle session(WinHttpOpen(L"Sims2Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        why = "the network cannot be used (WinHTTP)";
        return false;
    }
    WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 60000);
    Handle connection(WinHttpConnect(session.h, host, parts.nPort, 0));
    const std::wstring object = std::wstring(path) + extra;
    Handle request(connection.h ? WinHttpOpenRequest(connection.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr);
    if (!request.h) {
        why = "no connection to " + host_text;
        return false;
    }
    const wchar_t *headers = L"Accept: */*\r\nUser-Agent: Sims2Launcher\r\n";
    if (!WinHttpSendRequest(request.h, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.h, nullptr)) {
        why = "no connection to " + host_text + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        why = "the server " + host_text + " answered " + std::to_string(status);
        return false;
    }
    wchar_t length_text[32] = {};
    DWORD length_size = sizeof length_text;
    std::uint64_t total = 0;
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length_text, &length_size, WINHTTP_NO_HEADER_INDEX))
        total = std::wcstoull(length_text, nullptr, 10);
    std::vector<char> buffer(1 << 16);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buffer.data(), static_cast<DWORD>(buffer.size()), &got)) {
            why = "the download from " + host_text + " broke off (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        if (got == 0) break;
        if (!sink(buffer.data(), got, total)) {
            why = "cancelled";
            return false;
        }
    }
    return true;
}
#endif
}  // namespace

bool http_get(const std::string &url, const std::string &to_file, const std::function<bool(std::uint64_t, std::uint64_t)> &progress, std::string &why) {
#ifdef _WIN32
    std::FILE *out = ps2::os::open(to_file, "wb");
    if (!out) {
        why = "cannot write " + to_file;
        return false;
    }
    std::uint64_t got = 0;
    bool write_failed = false;
    const bool ok = get(
        url,
        [&](const char *data, std::size_t n, std::uint64_t total) {
            if (std::fwrite(data, 1, n, out) != n) {
                write_failed = true;
                return false;
            }
            got += n;
            return !progress || progress(got, total);
        },
        why);
    const bool closed = std::fclose(out) == 0;
    if (write_failed || !closed) why = "the disk is full or cannot be written (" + to_file + ")";
    if (!ok || write_failed || !closed) {
        std::error_code e;
        std::filesystem::remove(ps2::os::path(to_file), e);
        return false;
    }
    return true;
#else
    (void)url;
    (void)to_file;
    (void)progress;
    why = "downloads are only written for Windows so far";
    return false;
#endif
}

bool http_get_text(const std::string &url, std::string &out, std::string &why) {
#ifdef _WIN32
    out.clear();
    return get(
        url,
        [&](const char *data, std::size_t n, std::uint64_t) {
            out.append(data, n);
            return out.size() < (8u << 20);
        },
        why);
#else
    (void)url;
    (void)out;
    why = "downloads are only written for Windows so far";
    return false;
#endif
}

std::string sha256_of_file(const std::string &file) {
#ifdef _WIN32
    std::FILE *in = ps2::os::open(file, "rb");
    if (!in) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buffer(1 << 16);
        std::size_t n;
        bool ok = true;
        while (ok && (n = std::fread(buffer.data(), 1, buffer.size(), in)) > 0) ok = BCryptHashData(hash, buffer.data(), static_cast<ULONG>(n), 0) == 0;
        unsigned char digest[32];
        if (ok && !std::ferror(in) && BCryptFinishHash(hash, digest, sizeof digest, 0) == 0) {
            static const char *kHex = "0123456789abcdef";
            for (unsigned char b : digest) {
                hex += kHex[b >> 4];
                hex += kHex[b & 15];
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    std::fclose(in);
    return hex;
#else
    (void)file;
    return {};
#endif
}
