// App Updater unit tests (no framework needed).
#include "../tools/app_updater/Platform.h"
#include "../tools/app_updater/UpdaterCore.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#if ! defined(_WIN32)
  #include <csignal>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace upd;

static int failures = 0;
#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (! (cond)) {                                         \
            ++failures;                                         \
            std::printf ("  FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf (__VA_ARGS__);                          \
            std::printf ("\n");                                 \
        }                                                       \
    } while (0)

static fs::path scratch;

static std::string readAll (const fs::path& p)
{
    std::ifstream f (p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void writeAll (const fs::path& p, const std::string& s)
{
    fs::create_directories (p.parent_path());
    std::ofstream f (p, std::ios::binary | std::ios::trunc);
    f << s;
}

/** A minimal file that passes as a Windows program, with `payload` after the header. */
static std::string fakePE (const std::string& payload)
{
    std::string s (64, '\0');
    s[0] = 'M'; s[1] = 'Z';
    s[60] = 64; // e_lfanew
    s += std::string ("PE\0\0", 4);
    s += payload;
    return s;
}

static std::string fakeELF (const std::string& payload) { return std::string ("\x7f" "ELF", 4) + payload; }

static fs::path freshDir (const std::string& name)
{
    auto d = scratch / name;
    fs::remove_all (d);
    fs::create_directories (d);
    return d;
}

static Options testOpts (const fs::path& dir)
{
    Options o;
    o.backupRoot = dir / "backups";
    o.lockRetrySeconds = 1;
    return o;
}

static int countLeftovers (const fs::path& dir)
{
    int n = 0;
    for (auto& e : fs::directory_iterator (dir))
        if (e.path().extension() == ".tmp")
            ++n;
    return n;
}

// ---------------------------------------------------------------------------------------------
static void testSha256()
{
    std::printf ("sha256\n");
    CHECK (sha256 ("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty");
    CHECK (sha256 ("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    const std::string m (1000000, 'a');
    CHECK (sha256 (m.data(), m.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "million a");

    auto d = freshDir ("sha");
    std::mt19937 rng (3);
    std::string big (300000, '\0');
    for (auto& c : big)
        c = (char) (rng() & 0xff);
    writeAll (d / "f.bin", big);
    CHECK (sha256File (d / "f.bin") == sha256 (big.data(), big.size()), "file hash matches memory hash");
    CHECK (sha256File (d / "missing.bin").empty(), "missing file -> empty");
}

static void testDetect()
{
    std::printf ("detect executable\n");
    auto d = freshDir ("detect");
    writeAll (d / "a.exe", fakePE ("x"));
    writeAll (d / "b", fakeELF ("x"));
    writeAll (d / "c.sh", "#!/bin/sh\necho hi\n");
    writeAll (d / "d.txt", "hello world, this is just text and not a program at all......................");
    writeAll (d / "e.exe", std::string ("MZ") + std::string (100, '\0')); // MZ without PE header
    CHECK (detectExecutable (d / "a.exe") == ExeKind::WindowsPE, "PE");
    CHECK (detectExecutable (d / "b") == ExeKind::ELF, "ELF");
    CHECK (detectExecutable (d / "c.sh") == ExeKind::Script, "script");
    CHECK (detectExecutable (d / "d.txt") == ExeKind::Unknown, "text");
    CHECK (detectExecutable (d / "e.exe") == ExeKind::Unknown, "bare MZ");
}

static void testBasicUpdate()
{
    std::printf ("basic update + backup\n");
    auto d = freshDir ("basic");
    const auto target = d / "app" / "MyApp.exe";
    const auto incoming = d / "downloads" / "MyApp-2.0-setup-free.exe"; // different name is fine
    writeAll (target, fakePE ("version 1"));
    writeAll (incoming, fakePE ("version 2 is longer"));
    writeAll (d / "app" / "data.dll", "keep me");

    auto o = testOpts (d);
    std::vector<std::string> log;
    o.log = [&] (const std::string& s) { log.push_back (s); };
    auto r = update (target, incoming, o);
    CHECK (r.ok && r.changed, "update ok: %s", r.message.c_str());
    CHECK (readAll (target) == fakePE ("version 2 is longer"), "target has new content");
    CHECK (fs::exists (incoming), "new file left where it was");
    CHECK (readAll (d / "app" / "data.dll") == "keep me", "other files untouched");
    CHECK (countLeftovers (d / "app") == 0, "no temp files left");
    CHECK (! r.backupFile.empty() && readAll (r.backupFile) == fakePE ("version 1"), "backup holds old version");
    CHECK (r.oldHash != r.newHash && r.newHash == sha256File (target), "hashes");
    CHECK (! log.empty(), "logged progress");

    auto backups = listBackups (target, o);
    CHECK (backups.size() == 1 && backups[0].file == r.backupFile, "one backup listed (%zu)", backups.size());
    CHECK (fs::exists (backupDirFor (target, o) / "origin.txt"), "origin recorded");
}

static void testIdentical()
{
    std::printf ("identical file\n");
    auto d = freshDir ("same");
    const auto target = d / "App.exe";
    writeAll (target, fakePE ("same"));
    writeAll (d / "new" / "App.exe", fakePE ("same"));
    auto o = testOpts (d);
    auto r = update (target, d / "new" / "App.exe", o);
    CHECK (r.ok && ! r.changed, "identical -> ok, unchanged");
    CHECK (listBackups (target, o).empty(), "no backup for no-op");

    o.force = true;
    r = update (target, d / "new" / "App.exe", o);
    CHECK (r.ok && r.changed, "force replaces anyway");
}

static void testRejects()
{
    std::printf ("rejections\n");
    auto d = freshDir ("reject");
    const auto target = d / "App.exe";
    writeAll (target, fakePE ("v1"));
    writeAll (d / "notes.txt", "this is not a program, it is a text file with words in it.......................");
    writeAll (d / "linux_app", fakeELF ("v2"));
    auto o = testOpts (d);

    auto r = update (target, d / "notes.txt", o);
    CHECK (! r.ok, "text file rejected");
    r = update (target, d / "linux_app", o);
    CHECK (! r.ok, "wrong platform rejected");
    r = update (target, target, o);
    CHECK (! r.ok, "same file rejected");
    r = update (d / "nope.exe", d / "linux_app", o);
    CHECK (! r.ok, "missing target rejected");
    r = update (target, d / "nope.exe", o);
    CHECK (! r.ok, "missing new file rejected");
    CHECK (readAll (target) == fakePE ("v1"), "target untouched after rejections");
    CHECK (listBackups (target, o).empty(), "no backups after rejections");

    o.requireExecutable = false;
    r = update (target, d / "notes.txt", o);
    CHECK (r.ok && readAll (target) == readAll (d / "notes.txt"), "--any-file allows it");
}

static void testRestoreAndRotate()
{
    std::printf ("restore + rotation\n");
    auto d = freshDir ("restore");
    const auto target = d / "App.exe";
    writeAll (target, fakePE ("v1"));
    auto o = testOpts (d);
    o.maxBackups = 3;

    for (int v = 2; v <= 6; ++v)
    {
        writeAll (d / "dl" / "App.exe", fakePE ("v" + std::to_string (v)));
        auto r = update (target, d / "dl" / "App.exe", o);
        CHECK (r.ok && r.changed, "update to v%d: %s", v, r.message.c_str());
    }
    CHECK (readAll (target) == fakePE ("v6"), "at v6");
    auto backups = listBackups (target, o);
    CHECK (backups.size() == 3, "rotated to 3 backups (%zu)", backups.size());
    CHECK (! backups.empty() && readAll (backups[0].file) == fakePE ("v5"), "newest backup is v5");
    CHECK (backups.size() == 3 && readAll (backups[2].file) == fakePE ("v3"), "oldest kept is v3");

    auto r = restore (target, {}, o);
    CHECK (r.ok && r.changed, "restore newest: %s", r.message.c_str());
    CHECK (readAll (target) == fakePE ("v5"), "back at v5");
    backups = listBackups (target, o);
    CHECK (! backups.empty() && readAll (backups[0].file) == fakePE ("v6"), "v6 was backed up by the restore");

    // Restore the oldest one: it must not be rotated away mid-restore.
    const auto oldest = backups.back().file;
    const auto oldestContent = readAll (oldest);
    r = restore (target, oldest, o);
    CHECK (r.ok && readAll (target) == oldestContent, "restore specific backup");
    CHECK (countLeftovers (d) == 0, "no temp files left");

    auto none = freshDir ("restore_none");
    writeAll (none / "X.exe", fakePE ("x"));
    r = restore (none / "X.exe", {}, testOpts (none));
    CHECK (! r.ok, "restore with no backups fails cleanly");
}

static void testNoBackup()
{
    std::printf ("no backup\n");
    auto d = freshDir ("nobackup");
    writeAll (d / "App.exe", fakePE ("1"));
    writeAll (d / "new.exe", fakePE ("2"));
    auto o = testOpts (d);
    o.keepBackup = false;
    auto r = update (d / "App.exe", d / "new.exe", o);
    CHECK (r.ok && r.backupFile.empty() && ! fs::exists (d / "backups"), "nothing backed up");
}

static void testLeftovers()
{
    std::printf ("leftover cleanup\n");
    auto d = freshDir ("leftover");
    writeAll (d / "App.exe", fakePE ("1"));
    writeAll (d / "App.exe.old-20250101-000000-1.tmp", "stale");
    writeAll (d / "App.exe.new-update.tmp", "stale");
    writeAll (d / "Other.exe.old-20250101-000000-1.tmp", "not ours");
    writeAll (d / "new.exe", fakePE ("2"));
    auto r = update (d / "App.exe", d / "new.exe", testOpts (d));
    CHECK (r.ok, "update ok");
    CHECK (! fs::exists (d / "App.exe.old-20250101-000000-1.tmp") && ! fs::exists (d / "App.exe.new-update.tmp"), "stale removed");
    CHECK (fs::exists (d / "Other.exe.old-20250101-000000-1.tmp"), "other app's files untouched");
}

#if ! defined(_WIN32)
static void testPermissions()
{
    std::printf ("permissions kept\n");
    auto d = freshDir ("perms");
    writeAll (d / "app", fakeELF ("1"));
    writeAll (d / "new", fakeELF ("2"));
    fs::permissions (d / "app", fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
    fs::permissions (d / "new", fs::perms::owner_read | fs::perms::owner_write);
    auto r = update (d / "app", d / "new", testOpts (d));
    CHECK (r.ok, "update ok");
    const auto p = fs::status (d / "app").permissions();
    CHECK ((p & fs::perms::owner_exec) != fs::perms::none, "still executable");
}

static void testRunningApp()
{
  #if defined(__linux__)
    std::printf ("running app (close + relaunch-free swap)\n");
    auto d = freshDir ("running");
    const auto target = d / "sleeper";
    fs::copy_file ("/bin/sleep", target);
    writeAll (d / "new_sleeper", readAll ("/bin/sleep") + std::string ("\nnew version"));

    auto start = [&] {
        const pid_t pid = fork();
        if (pid == 0)
        {
            execl (target.c_str(), target.c_str(), "30", (char*) nullptr);
            _exit (127);
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (200));
        return pid;
    };

    // 1. running, not closing: Linux lets us swap anyway; the process keeps running.
    pid_t pid = start();
    auto pids = platform::findProcesses (target);
    CHECK (pids.size() == 1 && pids[0] == (unsigned long) pid, "found running process (%zu)", pids.size());
    auto o = testOpts (d);
    auto r = update (target, d / "new_sleeper", o);
    CHECK (r.ok && r.changed, "update while running: %s", r.message.c_str());
    CHECK (kill (pid, 0) == 0, "process still alive");
    kill (pid, SIGKILL);
    waitpid (pid, nullptr, 0);

    // 2. running, close it first.
    writeAll (d / "newer", readAll (target) + "!");
    pid = start();
    o.closeRunning = true;
    o.closeTimeoutSeconds = 3;
    r = update (target, d / "newer", o);
    CHECK (r.ok && r.changed, "update with close: %s", r.message.c_str());
    int status = 0;
    const bool reaped = waitpid (pid, &status, WNOHANG) == pid || kill (pid, 0) != 0;
    CHECK (reaped, "process was closed");
    if (! reaped) { kill (pid, SIGKILL); waitpid (pid, nullptr, 0); }
    CHECK (platform::findProcesses (target).empty(), "nothing running now");
  #endif
}

static void testReadOnlyDir()
{
    if (geteuid() == 0)
    {
        std::printf ("read-only folder (skipped: running as root)\n");
        return;
    }
    std::printf ("read-only folder\n");
    auto d = freshDir ("readonly");
    writeAll (d / "app" / "App.exe", fakePE ("1"));
    writeAll (d / "App2.exe", fakePE ("2"));
    fs::permissions (d / "app", fs::perms::owner_read | fs::perms::owner_exec);
    auto r = update (d / "app" / "App.exe", d / "App2.exe", testOpts (d));
    fs::permissions (d / "app", fs::perms::owner_all);
    CHECK (! r.ok && r.needsElevation, "fails asking for elevation");
    CHECK (readAll (d / "app" / "App.exe") == fakePE ("1"), "original intact");
    CHECK (countLeftovers (d / "app") == 0, "no temp files");
}
#endif

int main()
{
    scratch = fs::temp_directory_path() / ("app_updater_tests_" + std::to_string (std::random_device{}()));
    fs::create_directories (scratch);

    testSha256();
    testDetect();
    testBasicUpdate();
    testIdentical();
    testRejects();
    testRestoreAndRotate();
    testNoBackup();
    testLeftovers();
#if ! defined(_WIN32)
    testPermissions();
    testRunningApp();
    testReadOnlyDir();
#endif

    std::error_code ec;
    fs::remove_all (scratch, ec);
    if (failures)
    {
        std::printf ("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf ("\nall updater tests passed\n");
    return 0;
}
