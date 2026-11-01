# App Updater

App Updater swaps an installed program's `.exe` for a newer one. It backs up the old version first, so you can roll back.

You pick the installed app (for example `C:\Program Files\MyApp\MyApp.exe`) and the new `.exe` you downloaded or built. App Updater then:

1. **Checks** both files. It refuses anything that isn't a program, a program for a different OS, or the same file twice. If the two files are identical, it reports "already up to date".
2. **Closes the app** if it's running (optional). It asks the app to close, and forces it to quit after 10 seconds.
3. **Backs up** the current exe to your user folder and checks the copy.
4. **Swaps in the new exe.** The new file is copied next to the old one, then renamed into place. The installed app keeps its name and location, even if the new file has a different name, like `MyApp-2.1-x64.exe`. Other files in the app folder (DLLs, settings) are not touched.
5. **Verifies** that the result matches the new file byte-for-byte (SHA-256).
6. **Rolls back** automatically if any step fails, so you are never left with a broken install.
7. **Restarts the app** (optional).

## Getting it

* **Windows:** download `AppUpdater-Windows.exe` from the *Windows test build* release. It is portable, so there is nothing to install. Windows SmartScreen may warn because the file isn't code-signed. Click *More info → Run anyway*.
* **Any OS:** every CI run uploads `AppUpdater/` in the build artifacts. It contains the desktop app and `app_updater_cli`.
* **Build it yourself:** `cmake -B build && cmake --build build --config Release --target AppUpdater app_updater_cli`

## Using the desktop app

1. **Installed app:** click *Browse…* (or drag the exe onto the window), or pick an app from **Recent apps**.
2. **New version:** pick the new `.exe`.
3. Choose the options: **Keep a backup** (recommended), **Close the app if it's running**, and **Start it when done**.
4. Click **Update Now** and confirm. The confirmation shows both versions and warns you if the new file is older.

Apps under `C:\Program Files` need administrator rights to change. If App Updater needs them, it offers to restart itself as administrator and finish the update.

**Restore a Previous Version…** lists the backups of the selected app, newest first, and puts the one you pick back in place. The version being replaced is backed up too, so a restore can also be undone. **Open Backups Folder** shows the backup files.

## Command line

```
app_updater_cli "C:\Program Files\MyApp\MyApp.exe" "D:\Downloads\MyApp-2.1.exe" --close --relaunch
app_updater_cli --restore "C:\Program Files\MyApp\MyApp.exe"          # roll back to the previous version
app_updater_cli --list    "C:\Program Files\MyApp\MyApp.exe"          # show backups
```

| Option | Meaning |
|---|---|
| `--close` | Close the app first if it's running |
| `--relaunch` | Start the app again afterwards |
| `--no-backup` | Don't keep a copy of the old version |
| `--keep N` | Number of backups to keep per app (default 5, `0` = all) |
| `--backup-dir DIR` | Backup location (default: see below) |
| `--force` | Replace the exe even if the files are identical |
| `--any-file` | Allow files that don't look like programs |
| `--elevate` | (Windows) Show the administrator prompt if needed |
| `--quiet` | Print errors only |

Exit codes: `0` updated or already up to date, `1` failed (original kept), `2` bad arguments, `3` administrator rights needed.

## Where backups go

| OS | Folder |
|---|---|
| Windows | `%LOCALAPPDATA%\AppUpdater\Backups\<app>-<id>\` |
| macOS | `~/Library/Application Support/AppUpdater/Backups/` |
| Linux | `~/.local/share/app_updater/backups/` |

Each installed exe gets its own folder, named after the app plus a short id taken from its full path. Backups are named `MyApp-YYYYMMDD-HHMMSS.exe`, and the 5 newest are kept.

## Limits

* It replaces **one executable file**. It does not handle apps whose update also changes DLLs or data files, or apps that ship as an installer (`setup.exe`, `.msi`). For those, run the vendor's installer.
* On macOS, apps are `.app` folders, not single files. The tool can update the binary inside `MyApp.app/Contents/MacOS/`, but macOS may then reject the app's code signature.
