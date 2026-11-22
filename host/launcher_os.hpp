#pragma once

// Local change (the remaster): what the launcher needs from the system (Windows first; another system adds its own launcher_os file).

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Starts `exe` with `args` in `cwd` and waits until it has ended. false (and why) when it could not be started.
bool start_and_wait(const std::string &exe, const std::vector<std::string> &args, const std::string &cwd, int &exit_code, std::string &why);
// a process runs whose program file is `exe` (the same file, compared by full path)
bool runner_running(const std::string &exe);
// The borderless launcher window as a Windows layered window: `bgra` (premultiplied, 0xAARRGGBB, width x height) is its whole picture;
// clear pixels are see-through and let clicks pass. make_layered once after the window is made, then present_layered for every picture.
struct SDL_Window;
bool make_layered(SDL_Window *window);
bool present_layered(SDL_Window *window, const std::vector<std::uint32_t> &bgra, int width, int height);
// Starts `exe` with `args` in `cwd` with PATH replaced by `path_env` (the rest of the environment as it is), hands every line it writes
// (stdout and stderr) to on_line, and waits. `cancel` set: the program and everything it started are ended. false (and why) when it
// could not be started or was cancelled; exit_code otherwise.
bool run_with_output(const std::string &exe, const std::vector<std::string> &args, const std::string &cwd, const std::string &path_env,
                     const std::function<void(const std::string &line)> &on_line, const std::atomic<bool> &cancel, int &exit_code, std::string &why);
// free bytes on the drive of `dir` (0 when unknown)
std::uint64_t free_bytes(const std::string &dir);
// a directory junction `link` -> `target` (no administrator rights needed); removing a junction removes only the link
bool make_junction(const std::string &link, const std::string &target, std::string &why);
bool is_junction(const std::string &path);
bool remove_junction(const std::string &link);
// the folder of the running launcher (UTF-8)
std::string launcher_dir();
