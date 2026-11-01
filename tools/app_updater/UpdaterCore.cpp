#include "UpdaterCore.h"
#include "Platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>

namespace upd
{
// =============================================================================================
// SHA-256 (FIPS 180-4)
// =============================================================================================
namespace
{
struct Sha256
{
    std::uint32_t h[8] { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    std::uint8_t buf[64] {};
    std::uint64_t total = 0;
    size_t used = 0;

    static std::uint32_t rotr (std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block (const std::uint8_t* p)
    {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t) p[i * 4] << 24 | (std::uint32_t) p[i * 4 + 1] << 16
                 | (std::uint32_t) p[i * 4 + 2] << 8 | (std::uint32_t) p[i * 4 + 3];
        for (int i = 16; i < 64; ++i)
        {
            const auto s0 = rotr (w[i - 15], 7) ^ rotr (w[i - 15], 18) ^ (w[i - 15] >> 3);
            const auto s1 = rotr (w[i - 2], 17) ^ rotr (w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i)
        {
            const auto t1 = hh + (rotr (e, 6) ^ rotr (e, 11) ^ rotr (e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const auto t2 = (rotr (a, 2) ^ rotr (a, 13) ^ rotr (a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void add (const void* data, size_t n)
    {
        auto* p = static_cast<const std::uint8_t*> (data);
        total += n;
        while (n > 0)
        {
            const size_t take = std::min (n, 64 - used);
            std::memcpy (buf + used, p, take);
            used += take; p += take; n -= take;
            if (used == 64) { block (buf); used = 0; }
        }
    }

    std::string hex()
    {
        const std::uint64_t bits = total * 8;
        const std::uint8_t pad = 0x80, zero = 0;
        add (&pad, 1);
        while (used != 56)
            add (&zero, 1);
        std::uint8_t len[8];
        for (int i = 0; i < 8; ++i)
            len[i] = (std::uint8_t) (bits >> (56 - 8 * i));
        add (len, 8);
        static const char* digits = "0123456789abcdef";
        std::string out;
        for (auto v : h)
            for (int s = 28; s >= 0; s -= 4)
                out += digits[(v >> s) & 0xf];
        return out;
    }
};

std::string nowStamp()
{
    const std::time_t t = std::time (nullptr);
    std::tm tm {};
#if defined(_WIN32)
    localtime_s (&tm, &t);
#else
    localtime_r (&t, &tm);
#endif
    char b[32];
    std::strftime (b, sizeof (b), "%Y%m%d-%H%M%S", &tm);
    return b;
}

std::string u8 (const fs::path& p)
{
#if defined(__cpp_char8_t)
    auto s = p.u8string();
    return std::string (s.begin(), s.end());
#else
    return p.u8string();
#endif
}

[[maybe_unused]] std::string lower (std::string s)
{
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z')
            c = (char) (c - 'A' + 'a');
    return s;
}

std::string sanitize (const std::string& s)
{
    std::string out;
    for (char c : s)
        out += (std::isalnum ((unsigned char) c) || c == '-' || c == '_' || c == '.') ? c : '_';
    return out.empty() ? "app" : out;
}

bool isLockError (const std::error_code& ec)
{
    return ec == std::errc::permission_denied || ec == std::errc::device_or_resource_busy
        || ec == std::errc::text_file_busy || ec == std::errc::operation_not_permitted
#if defined(_WIN32)
        || ec.value() == 32 /*ERROR_SHARING_VIOLATION*/ || ec.value() == 33 /*ERROR_LOCK_VIOLATION*/
#endif
        ;
}

/** Retry `op` while it fails with a "file in use" error, up to `seconds`. */
template <typename Op>
bool retrying (int seconds, std::error_code& ec, Op op)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (std::max (0, seconds));
    for (;;)
    {
        ec.clear();
        op (ec);
        if (! ec)
            return true;
        if (! isLockError (ec) || std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for (std::chrono::milliseconds (250));
    }
}

bool copyVerified (const fs::path& from, const fs::path& to, const std::string& expectHash, std::error_code& ec)
{
    fs::copy_file (from, to, fs::copy_options::overwrite_existing, ec);
    if (ec)
        return false;
    if (sha256File (to) != expectHash)
    {
        ec = std::make_error_code (std::errc::io_error);
        return false;
    }
    return true;
}

std::string leftoverPrefix (const fs::path& target) { return u8 (target.filename()) + ".old-"; }
fs::path newTempFor (const fs::path& target) { return target.parent_path() / (target.filename().native() + fs::path (".new-update.tmp").native()); }

struct Logger
{
    const LogFn& fn;
    void operator() (const std::string& s) const { if (fn) fn (s); }
};

Result fail (Result r, const std::string& msg, const Logger& log)
{
    r.ok = false;
    r.message = msg;
    log ("ERROR: " + msg);
    return r;
}

void writeInfo (const fs::path& backup, const std::string& version, const std::string& hash, const fs::path& origin)
{
    std::ofstream f (backup.native() + fs::path (".info").native());
    f << "version=" << version << "\nsha256=" << hash << "\norigin=" << u8 (origin) << "\n";
}

std::string readInfoValue (const fs::path& backup, const std::string& key)
{
    std::ifstream f (backup.native() + fs::path (".info").native());
    std::string line;
    while (std::getline (f, line))
        if (line.rfind (key + "=", 0) == 0)
            return line.substr (key.size() + 1);
    return {};
}

void rotateBackups (const fs::path& target, const Options& opts, const fs::path& keep, const Logger& log)
{
    if (opts.maxBackups <= 0)
        return;
    auto all = listBackups (target, opts);
    int kept = 0;
    for (auto& b : all)
    {
        if (++kept <= opts.maxBackups || b.file == keep)
            continue;
        std::error_code ec;
        fs::remove (b.file, ec);
        fs::remove (b.file.native() + fs::path (".info").native(), ec);
        log ("Removed old backup " + u8 (b.file.filename()));
    }
}

Result doUpdate (const fs::path& targetIn, const fs::path& newIn, const Options& opts, const fs::path& protect)
{
    Result r;
    const Logger log { opts.log };
    std::error_code ec;

    const fs::path target = fs::absolute (targetIn, ec).lexically_normal();
    const fs::path newFile = fs::absolute (newIn, ec).lexically_normal();

    if (target.empty() || ! fs::is_regular_file (target, ec))
        return fail (r, "Installed app not found: " + u8 (target), log);
    if (newFile.empty() || ! fs::is_regular_file (newFile, ec))
        return fail (r, "New version not found: " + u8 (newFile), log);
    if (fs::equivalent (target, newFile, ec))
        return fail (r, "The new version is the same file as the installed app. Pick the downloaded file instead.", log);

    const auto oldKind = detectExecutable (target);
    const auto newKind = detectExecutable (newFile);
    if (opts.requireExecutable)
    {
        if (newKind == ExeKind::Unknown)
            return fail (r, u8 (newFile.filename()) + " does not look like a program.", log);
        if (oldKind != ExeKind::Unknown && oldKind != newKind)
            return fail (r, std::string ("The new file is a ") + toString (newKind) + " but the installed app is a "
                            + toString (oldKind) + ". It would not run here.", log);
    }

    cleanupLeftovers (target, opts.log);

    log ("Installed: " + u8 (target) + " (" + humanSize (fs::file_size (target, ec)) + ")");
    log ("New file:  " + u8 (newFile) + " (" + humanSize (fs::file_size (newFile, ec)) + ")");

    r.oldHash = sha256File (target);
    r.newHash = sha256File (newFile);
    if (r.oldHash.empty())
        return fail (r, "Could not read the installed app " + u8 (target), log);
    if (r.newHash.empty())
        return fail (r, "Could not read the new version " + u8 (newFile), log);
    r.oldVersion = platform::fileVersion (target);
    r.newVersion = platform::fileVersion (newFile);
    if (! r.oldVersion.empty() || ! r.newVersion.empty())
        log ("Version: " + (r.oldVersion.empty() ? "?" : r.oldVersion) + " -> " + (r.newVersion.empty() ? "?" : r.newVersion));

    if (r.oldHash == r.newHash && ! opts.force)
    {
        r.ok = true;
        r.message = "Already up to date: the installed app is identical to the new file.";
        log (r.message);
        return r;
    }

    // ---- running app --------------------------------------------------------------------
    auto pids = platform::findProcesses (target);
    if (! pids.empty())
    {
        if (opts.closeRunning)
        {
            log ("The app is running, asking it to close...");
            if (! platform::closeProcesses (pids, opts.closeTimeoutSeconds, opts.log))
                return fail (r, "The app is still running and could not be closed. Close it and try again.", log);
            log ("The app was closed.");
        }
        else
        {
            log ("Note: the app is running. It will use the new version the next time it starts.");
        }
    }

    // ---- backup -------------------------------------------------------------------------
    if (opts.keepBackup)
    {
        const auto dir = backupDirFor (target, opts);
        fs::create_directories (dir, ec);
        if (ec)
            return fail (r, "Could not create the backup folder " + u8 (dir) + ": " + ec.message(), log);
        {
            std::ofstream origin (dir / "origin.txt");
            origin << u8 (target) << "\n";
        }
        const auto stamp = nowStamp();
        // Never reuse a name (rotation may have freed one): continue after the highest sequence.
        int first = 1;
        for (auto& b : listBackups (target, opts))
            if (b.timestamp == stamp)
                first = std::max (first, b.sequence + 1);
        fs::path backup;
        for (int n = first;; ++n)
        {
            auto name = target.stem().native() + fs::path ("-" + stamp + (n > 1 ? "-" + std::to_string (n) : "")).native()
                      + target.extension().native();
            backup = dir / name;
            if (! fs::exists (backup, ec))
                break;
        }
        if (! copyVerified (target, backup, r.oldHash, ec))
        {
            fs::remove (backup, ec);
            return fail (r, "Could not back up the installed app: " + ec.message(), log);
        }
        writeInfo (backup, r.oldVersion, r.oldHash, target);
        r.backupFile = backup;
        log ("Backed up the current version to " + u8 (backup));
    }

    // ---- stage the new file next to the old one (same drive, so the swap is a rename) -----
    const auto staged = newTempFor (target);
    if (! copyVerified (newFile, staged, r.newHash, ec))
    {
        const bool denied = ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted
                            || ec == std::errc::read_only_file_system;
        std::error_code ec2;
        fs::remove (staged, ec2);
        r.needsElevation = denied && ! platform::isElevated();
        return fail (r, "Could not write to " + u8 (target.parent_path()) + ": " + ec.message()
                        + (r.needsElevation ? ". Run App Updater as administrator." : ""), log);
    }
    platform::copyPermissions (target, staged);

    // ---- swap -----------------------------------------------------------------------------
    fs::path old;
    for (int n = 1;; ++n)
    {
        old = target.parent_path() / fs::u8path (leftoverPrefix (target) + nowStamp() + "-" + std::to_string (n) + ".tmp");
        if (! fs::exists (old, ec))
            break;
    }

    if (! retrying (opts.lockRetrySeconds, ec, [&] (std::error_code& e) { fs::rename (target, old, e); }))
    {
        std::error_code ec2;
        fs::remove (staged, ec2);
        r.needsElevation = ec == std::errc::permission_denied && ! platform::isElevated();
        return fail (r, "The installed app is locked (" + ec.message() + "). Close it and try again"
                        + std::string (r.needsElevation ? ", or run App Updater as administrator." : "."), log);
    }

    auto rollback = [&] {
        std::error_code e;
        fs::remove (target, e);
        fs::rename (old, target, e);
        fs::remove (staged, e);
        return ! e;
    };

    if (! retrying (opts.lockRetrySeconds, ec, [&] (std::error_code& e) { fs::rename (staged, target, e); }))
    {
        const auto why = ec.message();
        const bool rolled = rollback();
        return fail (r, "Could not put the new version in place (" + why + "). "
                        + (rolled ? "The original was restored." : "WARNING: restoring the original failed; it is at " + u8 (old)), log);
    }

    if (sha256File (target) != r.newHash)
    {
        const bool rolled = rollback();
        return fail (r, std::string ("Verification failed after the update. ")
                        + (rolled ? "The original was restored." : "WARNING: restoring the original failed; it is at " + u8 (old)), log);
    }

    fs::remove (old, ec);
    if (ec)
        log ("The old version is still in use; its leftover file will be removed next time.");

    if (opts.keepBackup)
        rotateBackups (target, opts, protect, log);

    r.ok = true;
    r.changed = true;
    r.message = "Updated " + u8 (target.filename()) + " successfully"
              + (r.newVersion.empty() ? std::string() : " to version " + r.newVersion) + ".";
    log (r.message);

    if (opts.relaunch)
    {
        std::string err;
        if (platform::launch (target, err))
            log ("Started " + u8 (target.filename()));
        else
            log ("Could not start the app: " + err);
    }
    return r;
}
} // namespace

// =============================================================================================
const char* toString (ExeKind k)
{
    switch (k)
    {
        case ExeKind::WindowsPE: return "Windows program";
        case ExeKind::ELF:       return "Linux program";
        case ExeKind::MachO:     return "macOS program";
        case ExeKind::Script:    return "script";
        default:                 return "not a program";
    }
}

ExeKind detectExecutable (const fs::path& file)
{
    std::ifstream f (file, std::ios::binary);
    std::array<unsigned char, 64> h {};
    f.read (reinterpret_cast<char*> (h.data()), (std::streamsize) h.size());
    const auto n = f.gcount();
    if (n >= 64 && h[0] == 'M' && h[1] == 'Z')
    {
        // Follow e_lfanew to the "PE\0\0" signature.
        const std::uint32_t off = h[60] | h[61] << 8 | h[62] << 16 | (std::uint32_t) h[63] << 24;
        char sig[4] {};
        f.clear();
        f.seekg (off);
        f.read (sig, 4);
        if (f.gcount() == 4 && std::memcmp (sig, "PE\0\0", 4) == 0)
            return ExeKind::WindowsPE;
        return ExeKind::Unknown;
    }
    if (n >= 4 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F')
        return ExeKind::ELF;
    if (n >= 4)
    {
        const std::uint32_t be = (std::uint32_t) h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3];
        if (be == 0xfeedface || be == 0xfeedfacf || be == 0xcefaedfe || be == 0xcffaedfe || be == 0xcafebabe)
            return ExeKind::MachO;
    }
    if (n >= 2 && h[0] == '#' && h[1] == '!')
        return ExeKind::Script;
    return ExeKind::Unknown;
}

std::string sha256 (const void* data, size_t size)
{
    Sha256 s;
    s.add (data, size);
    return s.hex();
}

std::string sha256File (const fs::path& file)
{
    std::ifstream f (file, std::ios::binary);
    if (! f)
        return {};
    Sha256 s;
    std::vector<char> buf (1 << 16);
    while (f)
    {
        f.read (buf.data(), (std::streamsize) buf.size());
        if (f.gcount() > 0)
            s.add (buf.data(), (size_t) f.gcount());
    }
    if (f.bad())
        return {};
    return s.hex();
}

fs::path defaultBackupRoot()
{
#if defined(_WIN32)
    auto base = platform::envPath ("LOCALAPPDATA");
    if (base.empty())
        base = platform::envPath ("APPDATA");
    return base / "AppUpdater" / "Backups";
#elif defined(__APPLE__)
    return platform::envPath ("HOME") / "Library" / "Application Support" / "AppUpdater" / "Backups";
#else
    auto base = platform::envPath ("XDG_DATA_HOME");
    if (base.empty())
        base = platform::envPath ("HOME") / ".local" / "share";
    return base / "app_updater" / "backups";
#endif
}

fs::path backupDirFor (const fs::path& target, const Options& opts)
{
    std::error_code ec;
    const auto abs = fs::absolute (target, ec).lexically_normal();
    auto key = u8 (abs);
#if defined(_WIN32)
    key = lower (key); // Windows paths are case-insensitive
#endif
    const auto id = sha256 (key.data(), key.size()).substr (0, 10);
    const auto root = opts.backupRoot.empty() ? defaultBackupRoot() : opts.backupRoot;
    return root / (sanitize (u8 (abs.stem())) + "-" + id);
}

std::vector<BackupInfo> listBackups (const fs::path& target, const Options& opts)
{
    const auto dir = backupDirFor (target, opts);
    std::error_code ec;
    if (! fs::is_directory (dir, ec))
        return {};

    const auto stem = u8 (target.stem()) + "-";
    const auto ext = u8 (target.extension());
    std::vector<BackupInfo> items;
    for (auto& e : fs::directory_iterator (dir, ec))
    {
        if (! e.is_regular_file (ec))
            continue;
        const auto name = u8 (e.path().filename());
        if (name.size() <= stem.size() + ext.size() || name.rfind (stem, 0) != 0
            || name.compare (name.size() - ext.size(), ext.size(), ext) != 0)
            continue;
        const auto middle = name.substr (stem.size(), name.size() - stem.size() - ext.size()); // YYYYMMDD-HHMMSS[-n]
        if (middle.size() < 15 || middle[8] != '-')
            continue;
        bool digits = true;
        for (size_t i = 0; i < 15; ++i)
            if (i != 8 && ! std::isdigit ((unsigned char) middle[i]))
                digits = false;
        if (! digits)
            continue;
        int seq = 1;
        if (middle.size() > 16 && middle[15] == '-')
            seq = std::atoi (middle.c_str() + 16);
        else if (middle.size() != 15)
            continue;
        BackupInfo info;
        info.file = e.path();
        info.timestamp = middle.substr (0, 15);
        info.size = e.file_size (ec);
        info.version = readInfoValue (e.path(), "version");
        info.sequence = seq;
        items.push_back (info);
    }
    std::sort (items.begin(), items.end(), [] (const BackupInfo& a, const BackupInfo& b) {
        return a.timestamp != b.timestamp ? a.timestamp > b.timestamp : a.sequence > b.sequence;
    });
    return items;
}

Result update (const fs::path& target, const fs::path& newFile, const Options& opts)
{
    return doUpdate (target, newFile, opts, {});
}

Result restore (const fs::path& target, const fs::path& backup, const Options& opts)
{
    const Logger log { opts.log };
    fs::path from = backup;
    if (from.empty())
    {
        const auto all = listBackups (target, opts);
        if (all.empty())
            return fail ({}, "There are no backups of " + u8 (target.filename()) + " to restore.", log);
        from = all.front().file;
    }
    log ("Restoring " + u8 (from.filename()));
    Options o = opts;
    o.keepBackup = true;          // back up what is there now, so the restore can be undone
    o.requireExecutable = false;  // it was installed before; trust it
    auto r = doUpdate (target, from, o, from);
    if (r.ok && r.changed)
        r.message = "Restored " + u8 (target.filename()) + " from the backup of " + u8 (from.filename()) + ".";
    else if (r.ok)
        r.message = "The installed app already matches that backup.";
    return r;
}

void cleanupLeftovers (const fs::path& target, const LogFn& log)
{
    std::error_code ec;
    const auto dir = fs::absolute (target, ec).parent_path();
    const auto prefix = leftoverPrefix (target);
    for (auto& e : fs::directory_iterator (dir, ec))
    {
        const auto name = u8 (e.path().filename());
        const bool old = name.rfind (prefix, 0) == 0 && name.size() > 4 && name.compare (name.size() - 4, 4, ".tmp") == 0;
        const bool staged = e.path() == newTempFor (fs::absolute (target, ec));
        if (! old && ! staged)
            continue;
        std::error_code rm;
        if (fs::remove (e.path(), rm) && log)
            log ("Removed leftover " + name);
    }
}

std::string humanSize (std::uintmax_t bytes)
{
    const char* units[] = { "bytes", "KB", "MB", "GB" };
    double v = (double) bytes;
    int u = 0;
    while (v >= 1024.0 && u < 3) { v /= 1024.0; ++u; }
    char b[32];
    std::snprintf (b, sizeof (b), u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return b;
}
} // namespace upd
