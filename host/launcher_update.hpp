#pragma once

// Local change (the remaster): the launcher's update check - the newest release of the remaster on GitHub. It never blocks Play: a
// check that fails (offline, rate limit) just finds nothing.

#include <string>

constexpr const char *kLatestReleaseUrl = "https://api.github.com/repos/gabe4tw/TheSims2-PS2-Recomp/releases/latest";

// a is a newer version than b ("v1.10" > "v1.2"; a leading v and missing trailing .0 do not count); false for an empty a
bool newer(const std::string &a, const std::string &b);
// "tag_name" of a GitHub release JSON answer ("" when there is none)
std::string release_tag(const std::string &json);
// the newest release's tag ("" and why when it could not be found)
std::string latest_release(const std::string &url, std::string &why);
