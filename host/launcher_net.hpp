#pragma once

// Local change (the remaster): what the launcher needs from the network and for checking files (Windows: WinHTTP and CNG).

#include <cstdint>
#include <functional>
#include <string>

// GET `url` (http or https, redirects followed) into `to_file`. progress(got, total or 0) returning false cancels. On any failure the
// file is removed and `why` says what happened in plain words ("the server answered 404", "no connection to github.com").
bool http_get(const std::string &url, const std::string &to_file, const std::function<bool(std::uint64_t, std::uint64_t)> &progress, std::string &why);
// GET `url` into memory (small answers, e.g. the GitHub API); false + why on failure
bool http_get_text(const std::string &url, std::string &out, std::string &why);
// the sha256 of a file as lower-case hex; "" when it cannot be read
std::string sha256_of_file(const std::string &file);
