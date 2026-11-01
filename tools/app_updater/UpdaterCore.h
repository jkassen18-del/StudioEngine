// App Updater - core engine (framework-free, C++17).
//
// Replaces an installed program's executable with a newer one, safely:
//
//   1. checks both files (exists, is a real program, not the same file, not identical)
//   2. closes the running app if asked (Windows/Linux)
//   3. backs up the installed exe to a backup folder (timestamped, rotated)
//   4. copies the new exe next to the old one as a temporary file
//   5. renames the old exe out of the way (this works even while it is running on Windows)
//   6. renames the new exe into place, then verifies it byte-for-byte (SHA-256)
//   7. on any failure, puts the original back exactly as it was
//
// Every installed exe has its own backup history, so you can roll back with restore().
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace upd
{
namespace fs = std::filesystem;

enum class ExeKind { Unknown, WindowsPE, ELF, MachO, Script };
const char* toString (ExeKind k);

/** Looks at the first bytes of a file to tell what kind of program it is. */
ExeKind detectExecutable (const fs::path& file);

/** SHA-256 of a file as lowercase hex, or empty on read error. */
std::string sha256File (const fs::path& file);

/** SHA-256 of a memory block as lowercase hex (exposed for tests). */
std::string sha256 (const void* data, size_t size);

struct BackupInfo
{
    fs::path file;
    std::string timestamp;  // YYYYMMDD-HHMMSS
    std::string version;    // file version, when the platform can read it
    std::uintmax_t size = 0;
    int sequence = 1;       // several backups within the same second
};

using LogFn = std::function<void (const std::string&)>;

struct Options
{
    bool keepBackup = true;         // copy the old exe to the backup folder first
    int  maxBackups = 5;            // older backups beyond this are deleted (0 = keep all)
    fs::path backupRoot;            // empty = per-user default (see defaultBackupRoot)
    bool requireExecutable = true;  // refuse files that do not look like programs
    bool force = false;             // update even when the new file is identical
    bool closeRunning = false;      // ask the running app to close (then kill after timeout)
    int  closeTimeoutSeconds = 10;
    int  lockRetrySeconds = 10;     // keep retrying while the exe is locked
    bool relaunch = false;          // start the app again afterwards
    LogFn log;                      // progress messages
};

struct Result
{
    bool ok = false;
    bool changed = false;         // false when nothing needed doing
    bool needsElevation = false;  // failed because of missing admin rights
    std::string message;
    fs::path backupFile;
    std::string oldHash, newHash;
    std::string oldVersion, newVersion;
};

/** Per-user backup folder:
    Windows  %LOCALAPPDATA%\AppUpdater\Backups
    macOS    ~/Library/Application Support/AppUpdater/Backups
    Linux    $XDG_DATA_HOME/app_updater/backups (~/.local/share/...) */
fs::path defaultBackupRoot();

/** Folder holding the backups of one particular installed exe. */
fs::path backupDirFor (const fs::path& target, const Options& opts);

/** Replace `target` (the installed exe) with `newFile`. */
Result update (const fs::path& target, const fs::path& newFile, const Options& opts = {});

/** Backups of `target`, newest first. */
std::vector<BackupInfo> listBackups (const fs::path& target, const Options& opts = {});

/** Put a backup back in place of `target`. Empty `backup` = the newest one.
    The current exe is itself backed up first, so a restore can be undone too. */
Result restore (const fs::path& target, const fs::path& backup = {}, const Options& opts = {});

/** Remove leftover temporary files from an earlier interrupted update. */
void cleanupLeftovers (const fs::path& target, const LogFn& log = {});

std::string humanSize (std::uintmax_t bytes);
} // namespace upd
