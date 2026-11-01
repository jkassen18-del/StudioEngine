// app_updater_cli - replace an installed program's exe with a new one from the command line.
#include "Platform.h"
#include "UpdaterCore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#if defined(_WIN32)
  #include <windows.h>
#endif

namespace fs = std::filesystem;

// UTF-8 text of a path (path::string() can throw on Windows for non-ANSI names).
static std::string str (const fs::path& p) { return p.u8string(); }

static void usage()
{
    std::puts (
        "App Updater - replace an installed program with a new version, with automatic backup.\n"
        "\n"
        "Usage:\n"
        "  app_updater_cli <installed.exe> <new.exe> [options]     update\n"
        "  app_updater_cli --restore <installed.exe> [backup]      roll back (newest backup by default)\n"
        "  app_updater_cli --list <installed.exe>                  show backups\n"
        "\n"
        "Options:\n"
        "  --close            close the app first if it is running\n"
        "  --relaunch         start the app again when done\n"
        "  --no-backup        do not keep a copy of the old version\n"
        "  --keep N           how many backups to keep (default 5, 0 = all)\n"
        "  --backup-dir DIR   where backups go (default: per-user app data folder)\n"
        "  --force            replace even if the files are identical\n"
        "  --any-file         allow files that do not look like programs\n"
        "  --elevate          (Windows) ask for administrator rights if needed\n"
        "  --quiet            only print errors\n"
        "\n"
        "Exit codes: 0 updated / already up to date, 1 failed, 2 bad arguments, 3 needs administrator.");
}

#if defined(_WIN32)
int wmain (int argc, wchar_t** argv)
{
    SetConsoleOutputCP (CP_UTF8);
    std::vector<fs::path> rawArgs;
    for (int i = 1; i < argc; ++i)
        rawArgs.emplace_back (argv[i]);
#else
int main (int argc, char** argv)
{
    std::vector<fs::path> rawArgs;
    for (int i = 1; i < argc; ++i)
        rawArgs.emplace_back (argv[i]);
#endif
    enum class Mode { Update, Restore, List } mode = Mode::Update;
    upd::Options opts;
    bool quiet = false, elevate = false;
    std::vector<fs::path> positional;

    for (size_t i = 0; i < rawArgs.size(); ++i)
    {
        const auto a = str (rawArgs[i]);
        auto next = [&] () -> fs::path {
            if (i + 1 >= rawArgs.size())
            {
                std::fprintf (stderr, "missing value after %s\n", a.c_str());
                std::exit (2);
            }
            return rawArgs[++i];
        };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--restore" || a == "--rollback") mode = Mode::Restore;
        else if (a == "--list") mode = Mode::List;
        else if (a == "--close") opts.closeRunning = true;
        else if (a == "--relaunch") opts.relaunch = true;
        else if (a == "--no-backup") opts.keepBackup = false;
        else if (a == "--keep") opts.maxBackups = std::atoi (str (next()).c_str());
        else if (a == "--backup-dir") opts.backupRoot = next();
        else if (a == "--force") opts.force = true;
        else if (a == "--any-file") opts.requireExecutable = false;
        else if (a == "--elevate") elevate = true;
        else if (a == "--quiet") quiet = true;
        else if (a.size() > 1 && a[0] == '-' && a[1] == '-') { std::fprintf (stderr, "unknown option %s\n\n", a.c_str()); usage(); return 2; }
        else positional.push_back (rawArgs[i]);
    }

    if (! quiet)
        opts.log = [] (const std::string& s) { std::cout << s << std::endl; };

    if (mode == Mode::List)
    {
        if (positional.size() != 1) { usage(); return 2; }
        const auto backups = upd::listBackups (positional[0], opts);
        std::cout << "Backups of " << str (positional[0]) << " (" << str (upd::backupDirFor (positional[0], opts)) << "):\n";
        if (backups.empty())
            std::cout << "  (none)\n";
        for (auto& b : backups)
            std::cout << "  " << str (b.file.filename()) << "  " << upd::humanSize (b.size)
                      << (b.version.empty() ? "" : "  v" + b.version) << "\n";
        return 0;
    }

    upd::Result r;
    if (mode == Mode::Restore)
    {
        if (positional.empty() || positional.size() > 2) { usage(); return 2; }
        r = upd::restore (positional[0], positional.size() == 2 ? positional[1] : fs::path(), opts);
    }
    else
    {
        if (positional.size() != 2) { usage(); return 2; }
        r = upd::update (positional[0], positional[1], opts);
    }

    if (! r.ok && r.needsElevation)
    {
#if defined(_WIN32)
        if (elevate)
        {
            std::vector<std::wstring> args;
            for (auto& p : rawArgs)
                if (p.wstring() != L"--elevate")
                    args.push_back (p.wstring());
            if (upd::platform::relaunchElevated (upd::platform::currentExecutable(), args))
            {
                std::cout << "Continuing in an administrator window." << std::endl;
                return 0;
            }
        }
        std::cerr << "Tip: run this from an administrator prompt, or add --elevate." << std::endl;
#else
        (void) elevate;
        std::cerr << "Tip: run it again with sudo." << std::endl;
#endif
        return 3;
    }
    if (! r.ok && quiet)
        std::cerr << r.message << std::endl;
    return r.ok ? 0 : 1;
}
