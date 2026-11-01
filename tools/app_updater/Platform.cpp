#include "Platform.h"

#include <chrono>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <shellapi.h>
  #include <tlhelp32.h>
  #pragma comment(lib, "version.lib")
  #pragma comment(lib, "shell32.lib")
  #pragma comment(lib, "user32.lib")
  #pragma comment(lib, "advapi32.lib")
#else
  #include <csignal>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #if defined(__APPLE__)
    #include <mach-o/dyld.h>
  #else
    #include <dirent.h>
  #endif
#endif

namespace upd::platform
{
using namespace std::chrono_literals;

static bool samePath (const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    if (fs::equivalent (a, b, ec))
        return true;
#if defined(_WIN32)
    return _wcsicmp (fs::absolute (a, ec).lexically_normal().c_str(),
                     fs::absolute (b, ec).lexically_normal().c_str()) == 0;
#else
    return fs::absolute (a, ec).lexically_normal() == fs::absolute (b, ec).lexically_normal();
#endif
}

fs::path envPath (const char* name)
{
#if defined(_WIN32)
    std::wstring wname (name, name + std::char_traits<char>::length (name));
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW (wname.c_str(), buf, 32768);
    return (n > 0 && n < 32768) ? fs::path (std::wstring (buf, n)) : fs::path();
#else
    const char* v = std::getenv (name);
    return (v && *v) ? fs::path (v) : fs::path();
#endif
}

// =============================================================================================
#if defined(_WIN32)

std::vector<unsigned long> findProcesses (const fs::path& exe)
{
    std::vector<unsigned long> pids;
    HANDLE snap = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return pids;

    const std::wstring wantName = exe.filename().wstring();
    PROCESSENTRY32W pe {};
    pe.dwSize = sizeof (pe);
    for (BOOL more = Process32FirstW (snap, &pe); more; more = Process32NextW (snap, &pe))
    {
        if (pe.th32ProcessID == GetCurrentProcessId() || _wcsicmp (pe.szExeFile, wantName.c_str()) != 0)
            continue;

        HANDLE h = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (! h)
        {
            pids.push_back (pe.th32ProcessID); // same name, can't check path: assume it's ours
            continue;
        }
        wchar_t buf[32768];
        DWORD len = 32768;
        if (QueryFullProcessImageNameW (h, 0, buf, &len) && samePath (fs::path (std::wstring (buf, len)), exe))
            pids.push_back (pe.th32ProcessID);
        CloseHandle (h);
    }
    CloseHandle (snap);
    return pids;
}

static BOOL CALLBACK closeWindowProc (HWND hwnd, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId (hwnd, &pid);
    if (pid == (DWORD) lp)
        PostMessageW (hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

static bool isRunning (unsigned long pid)
{
    HANDLE h = OpenProcess (SYNCHRONIZE, FALSE, pid);
    if (! h)
        return false;
    const bool running = WaitForSingleObject (h, 0) == WAIT_TIMEOUT;
    CloseHandle (h);
    return running;
}

bool closeProcesses (const std::vector<unsigned long>& pids, int timeoutSeconds,
                     const std::function<void (const std::string&)>& log)
{
    for (auto pid : pids)
        EnumWindows (closeWindowProc, (LPARAM) pid);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (timeoutSeconds);
    auto anyRunning = [&] {
        for (auto pid : pids)
            if (isRunning (pid))
                return true;
        return false;
    };
    while (anyRunning() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (200ms);

    bool allGone = true;
    for (auto pid : pids)
    {
        if (! isRunning (pid))
            continue;
        if (log)
            log ("The app did not close by itself, forcing it to quit (pid " + std::to_string (pid) + ")");
        HANDLE h = OpenProcess (PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (h && TerminateProcess (h, 1))
            WaitForSingleObject (h, 5000);
        else
            allGone = false;
        if (h)
            CloseHandle (h);
    }
    return allGone && ! anyRunning();
}

bool launch (const fs::path& exe, std::string& error)
{
    const std::wstring dir = exe.parent_path().wstring();
    auto r = (INT_PTR) ShellExecuteW (nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    if (r <= 32)
    {
        error = "could not start the app (error " + std::to_string ((long long) r) + ")";
        return false;
    }
    return true;
}

std::string fileVersion (const fs::path& file)
{
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW (file.c_str(), &handle);
    if (size == 0)
        return {};
    std::vector<unsigned char> data (size);
    if (! GetFileVersionInfoW (file.c_str(), 0, size, data.data()))
        return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (! VerQueryValueW (data.data(), L"\\", (void**) &info, &len) || ! info || len < sizeof (VS_FIXEDFILEINFO))
        return {};
    return std::to_string (HIWORD (info->dwFileVersionMS)) + "." + std::to_string (LOWORD (info->dwFileVersionMS)) + "."
         + std::to_string (HIWORD (info->dwFileVersionLS)) + "." + std::to_string (LOWORD (info->dwFileVersionLS));
}

bool isElevated()
{
    HANDLE token = nullptr;
    if (! OpenProcessToken (GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elev {};
    DWORD len = 0;
    const bool ok = GetTokenInformation (token, TokenElevation, &elev, sizeof (elev), &len) && elev.TokenIsElevated;
    CloseHandle (token);
    return ok;
}

bool relaunchElevated (const fs::path& exe, const std::vector<std::wstring>& args)
{
    std::wstring params;
    for (auto& a : args)
    {
        if (! params.empty())
            params += L' ';
        params += L'"';
        for (wchar_t c : a)
            if (c != L'"')
                params += c;
        params += L'"';
    }
    SHELLEXECUTEINFOW sei {};
    sei.cbSize = sizeof (sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW (&sei) != FALSE;
}

void copyPermissions (const fs::path&, const fs::path&) {}

fs::path currentExecutable()
{
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW (nullptr, buf, 32768);
    return fs::path (std::wstring (buf, n));
}

// =============================================================================================
#else // POSIX

std::vector<unsigned long> findProcesses (const fs::path& exe)
{
    std::vector<unsigned long> pids;
  #if defined(__linux__)
    DIR* d = opendir ("/proc");
    if (! d)
        return pids;
    const pid_t self = getpid();
    while (auto* e = readdir (d))
    {
        char* end = nullptr;
        const long pid = std::strtol (e->d_name, &end, 10);
        if (pid <= 0 || *end != '\0' || pid == self)
            continue;
        std::error_code ec;
        auto link = fs::read_symlink (fs::path ("/proc") / e->d_name / "exe", ec);
        if (ec)
            continue;
        // After the file is replaced, Linux shows "<path> (deleted)" for the old image.
        auto s = link.string();
        const std::string suffix = " (deleted)";
        if (s.size() > suffix.size() && s.compare (s.size() - suffix.size(), suffix.size(), suffix) == 0)
            s.resize (s.size() - suffix.size());
        if (samePath (s, exe))
            pids.push_back ((unsigned long) pid);
    }
    closedir (d);
  #else
    (void) exe;
  #endif
    return pids;
}

static bool isRunning (unsigned long pid)
{
    // Reap it if it happens to be our own child, otherwise kill(0) just checks existence.
    int status = 0;
    if (waitpid ((pid_t) pid, &status, WNOHANG) == (pid_t) pid)
        return false;
    if (kill ((pid_t) pid, 0) != 0)
        return false;
  #if defined(__linux__)
    // Zombies (exited, not yet reaped by their parent) count as gone.
    FILE* f = std::fopen (("/proc/" + std::to_string (pid) + "/stat").c_str(), "r");
    if (f)
    {
        int p = 0;
        char comm[512] = {}, state = 0;
        const int n = std::fscanf (f, "%d %511s %c", &p, comm, &state);
        std::fclose (f);
        if (n == 3 && state == 'Z')
            return false;
    }
  #endif
    return true;
}

bool closeProcesses (const std::vector<unsigned long>& pids, int timeoutSeconds,
                     const std::function<void (const std::string&)>& log)
{
    for (auto pid : pids)
        kill ((pid_t) pid, SIGTERM);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (timeoutSeconds);
    auto anyRunning = [&] {
        for (auto pid : pids)
            if (isRunning (pid))
                return true;
        return false;
    };
    while (anyRunning() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (100ms);

    for (auto pid : pids)
        if (isRunning (pid))
        {
            if (log)
                log ("The app did not close by itself, forcing it to quit (pid " + std::to_string (pid) + ")");
            kill ((pid_t) pid, SIGKILL);
        }
    for (int i = 0; i < 50 && anyRunning(); ++i)
        std::this_thread::sleep_for (100ms);
    return ! anyRunning();
}

bool launch (const fs::path& exe, std::string& error)
{
    const pid_t pid = fork();
    if (pid < 0)
    {
        error = "fork failed";
        return false;
    }
    if (pid == 0)
    {
        setsid();
        if (fork() == 0) // grandchild, so the app is not our child
        {
            const int devnull = open ("/dev/null", O_RDWR);
            if (devnull >= 0)
            {
                dup2 (devnull, 0);
                dup2 (devnull, 1);
                dup2 (devnull, 2);
            }
            if (chdir (exe.parent_path().c_str()) != 0) {}
            execl (exe.c_str(), exe.c_str(), (char*) nullptr);
            _exit (127);
        }
        _exit (0);
    }
    int status = 0;
    waitpid (pid, &status, 0);
    return true;
}

std::string fileVersion (const fs::path&) { return {}; }

bool isElevated() { return geteuid() == 0; }

bool relaunchElevated (const fs::path&, const std::vector<std::wstring>&) { return false; }

void copyPermissions (const fs::path& like, const fs::path& file)
{
    std::error_code ec;
    auto perms = fs::status (like, ec).permissions();
    if (! ec)
        fs::permissions (file, perms, fs::perm_options::replace, ec);
}

fs::path currentExecutable()
{
  #if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof (buf);
    if (_NSGetExecutablePath (buf, &size) == 0)
        return fs::path (buf);
    return {};
  #else
    std::error_code ec;
    return fs::read_symlink ("/proc/self/exe", ec);
  #endif
}

#endif
} // namespace upd::platform
