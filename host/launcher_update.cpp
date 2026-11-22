#include "launcher_update.hpp"

#include "launcher_net.hpp"

#include <cctype>
#include <vector>

namespace {
std::vector<long> numbers(const std::string &version) {
    std::vector<long> out;
    std::size_t i = 0;
    while (i < version.size() && !std::isdigit(static_cast<unsigned char>(version[i]))) ++i;
    while (i < version.size()) {
        long n = 0;
        bool any = false;
        while (i < version.size() && std::isdigit(static_cast<unsigned char>(version[i]))) {
            n = n * 10 + (version[i++] - '0');
            any = true;
        }
        if (any) out.push_back(n);
        if (i < version.size() && version[i] == '.') ++i;
        else break;
    }
    while (!out.empty() && out.back() == 0) out.pop_back();
    return out;
}
}  // namespace

bool newer(const std::string &a, const std::string &b) {
    if (a.empty()) return false;
    return numbers(a) > numbers(b);
}

std::string release_tag(const std::string &json) {
    const std::size_t key = json.find("\"tag_name\"");
    if (key == std::string::npos) return {};
    const std::size_t open = json.find('"', json.find(':', key) + 1);
    if (open == std::string::npos) return {};
    const std::size_t close = json.find('"', open + 1);
    return close == std::string::npos ? std::string() : json.substr(open + 1, close - open - 1);
}

std::string latest_release(const std::string &url, std::string &why) {
    std::string answer;
    if (!http_get_text(url, answer, why)) return {};
    const std::string tag = release_tag(answer);
    if (tag.empty()) why = "no release found";
    return tag;
}
