// Local change: the launcher's update check (host/launcher_update.cpp). Build: ninja ps2updatetest, run build/.../ps2updatetest.
#include "../host/launcher_update.hpp"

#include <cstdio>
#include <string>

namespace {
int failed = 0, checks = 0;
void check(bool ok, const std::string &what) {
    ++checks;
    if (!ok) {
        ++failed;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}
}  // namespace

int main() {
    check(newer("v1.10", "v1.2") && !newer("v1.2", "v1.10"), "newer_versions: 1.10 is newer than 1.2");
    check(!newer("1.0", "1.0.0") && !newer("1.0.0", "1.0"), "newer_versions: 1.0 and 1.0.0 are the same");
    check(newer("v2", "1.9.9") && !newer("", "v1"), "newer_versions: v prefix, empty");
    check(release_tag("{\"url\":\"x\",\"tag_name\": \"v1.2.3\",\"name\":\"Remaster 1.2.3\"}") == "v1.2.3", "release_tag_from_json");
    check(release_tag("{\"message\":\"API rate limit exceeded\"}").empty(), "release_tag_none");
    std::string why;
    const std::string got = latest_release("http://127.0.0.1:1/releases/latest", why);  // nothing listens there
    check(got.empty() && !why.empty(), "update_check_offline: no update, a reason for the log");
    std::printf("update: %d of %d checks passed, %d failed\n", checks - failed, checks, failed);
    return failed ? 1 : 0;
}
