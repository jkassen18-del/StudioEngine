// App Updater - operating-system specific helpers (process control, versions, elevation).
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace upd::platform
{
namespace fs = std::filesystem;

/** Process ids currently running `exe` (Windows and Linux; empty elsewhere). */
std::vector<unsigned long> findProcesses (const fs::path& exe);

/** Politely ask processes to quit, wait up to `timeoutSeconds`, then terminate them.
    Returns true when none of them are left running. */
bool closeProcesses (const std::vector<unsigned long>& pids, int timeoutSeconds,
                     const std::function<void (const std::string&)>& log);

/** Start a program detached from this one. */
bool launch (const fs::path& exe, std::string& error);

/** File version from the Windows version resource ("1.2.3.4"), empty if unavailable. */
std::string fileVersion (const fs::path& file);

/** True when running with administrator / root rights. */
bool isElevated();

/** Windows: run `exe` with `args` through a UAC prompt. Returns false if refused/unsupported. */
bool relaunchElevated (const fs::path& exe, const std::vector<std::wstring>& args);

/** Make `file` executable like `like` (POSIX permission bits; no-op on Windows). */
void copyPermissions (const fs::path& like, const fs::path& file);

/** Path of the running executable. */
fs::path currentExecutable();

/** Environment variable as a path (empty if unset). */
fs::path envPath (const char* name);
} // namespace upd::platform
