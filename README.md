# Plugin Checker for OBS Studio

English · [Deutsch](README.de.md)

An OBS plugin that lists all installed third-party plugins and checks GitHub for newer versions. It is meant above all for the move to **OBS 33**: plugin loading has changed there, and plugins in old locations only keep loading for a transition period (until OBS 34 according to the announcement; OBS itself only says "in the future").

Open it in OBS via **Tools → Plugin Checker**. If updates are available, the menu shows how many, e.g. "Plugin Checker (2 updates)".

The interface follows the OBS language and is available in English, German, Spanish, French, Italian, Portuguese, Polish, Dutch, Japanese, Korean and Simplified Chinese; other languages fall back to English.

![Dialog](docs/dialog.png)

## Installation

Download the file for your system from the [releases](../../releases) and quit OBS before installing. The Plugin Checker needs OBS 31.1 or newer.

**Windows, installer:** run `…-windows-x64-installer.exe` and pick your OBS folder (it suggests the installed OBS; for a portable OBS choose its folder). Uninstall it via Windows Settings → Apps. The installer is not signed, so Windows SmartScreen may say "Windows protected your PC": click **More info** → **Run anyway**.

**Windows, ZIP:** copy the content of `…-windows-x64.zip` (the folders `obs-plugins` and `data`) into your OBS folder, e.g. `C:\Program Files\obs-studio\` or your portable OBS folder. Windows merges the folders; nothing of OBS is overwritten.

**macOS:** open the `.pkg`, or extract the `.zip` into `~/Library/Application Support/obs-studio/plugins/`. If macOS blocks it, see [below](#macos-plugin-is-blocked).

**Linux:** extract the `.tar.xz` into `~/.config/obs-studio/plugins/`.

Then start OBS and open **Tools → Plugin Checker**.

## What it checks

| | |
|---|---|
| **Version** | Installed version from the OBS 33 `manifest.json`, the `Info.plist` (macOS) or the file version resource (Windows) |
| **Latest version** | The newest GitHub release with a direct download for your system (`.pkg` / `.exe` / `.deb`) |
| **Legacy** | Plugins in old locations/folder layouts that OBS 34 will no longer load (see below) |
| **Duplicate copies** | e.g. a new `.plugin` version plus an old copy under `/Library/…` |
| **Not loaded** | Plugins that are on disk but were not loaded by OBS or are disabled in the plugin manager |
| **Maintained?** | Archived repos and releases older than the current OBS major version |
| **OBS itself** | Installed vs. latest OBS version |
| **Used?** | Which scene collections and profiles use the plugin's sources, filters, transitions and encoders. Plugins whose sources are not used anywhere are marked |
| **Missing libraries** | Plugins that need DLLs that no longer exist (e.g. after an OBS update) and therefore don't load |
| **Leftovers** | Data folders without a plugin, e.g. from removed or renamed plugins |

The table shows one row per plugin with version, update status, whether it is used, and the **Future** column: *new location*, *legacy location* (still works, but will stop loading at some point), *does not load* (e.g. missing DLLs) or *leftovers*. All notes for the selected plugin are shown below the table.

**Downloads** match your system: on Windows an installer is offered, but for a **portable OBS** the ZIP is preferred, because installers only install into the regular OBS. Downloads for other CPUs (ARM, 32-bit) are skipped. The file that will be downloaded is shown below the plugin.

**Update…** (Windows) downloads the release ZIP from GitHub, verifies the checksum GitHub provides for it and extracts it into a work folder. It recognizes the old flat layout (`obs-plugins/64bit` + `data/obs-plugins/<name>`), the ProgramData layout (`<name>/bin/64bit` + `<name>/data`) and the OBS 33 layout (`<name>/<name>.dll` + `data`). Only the plugin's DLLs and its data folder are taken over, into the place where the plugin already is. A preview first shows every file that will be replaced or added. ZIPs with paths outside their folder, target paths outside the OBS folders, or files that belong to OBS or other plugins abort the update. Files are only swapped after OBS exits; the old files and `restore.ps1` are kept in `plugin_config/obs-plugin-checker/updates`.

**Files…** shows all files of a plugin: plugin, data, bundled DLLs, copies next to `obs64.exe` and its settings folder. **Remove…** moves them into a backup folder after OBS exits (`plugin_config/obs-plugin-checker/removed`), with `restore.ps1`/`restore.sh` to bring them back. Nothing is deleted, and DLLs that other plugins need stay where they are.

## macOS: plugin is blocked

The macOS packages are not signed and notarized by Apple (see [Support](#support) for why). So macOS may block them the first time you open them, for example with a message that Apple cannot check the file for malicious software. Here is how to get it installed anyway:

1. **Installer (`.pkg`):** Double-click the file once and close the message with **Done**. Then open **System Settings → Privacy & Security**, scroll down to the note that the file was blocked and click **Open Anyway**. After confirming with your password or Touch ID, the installer runs normally. (Up to macOS 14, right-click the file → **Open** → **Open** also works.)
2. **ZIP (`.plugin`):** Extract it to `~/Library/Application Support/obs-studio/plugins/`. If OBS does not load the plugin afterwards (it is missing under **Tools**), remove the download marker in Terminal and restart OBS:
   ```sh
   xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/obs-plugin-checker.plugin
   ```

The packages are built only by GitHub CI from the code in this repository. The checksums are in `SHA256SUMS.txt` on the release (compare with `shasum -a 256 <file>`).

## Privacy and security

- Scene collections are only searched for source types; names, settings and URLs are never shown, stored or logged. From profiles, only the encoder entries in `basic.ini` are read; the plugin never opens `service.json` (stream key), `global.ini` or `user.ini`.
- It only writes its version and the number of plugins and updates to the OBS log.
- Network: only HTTPS requests to `api.github.com`, without login and without tokens. GitHub learns which plugin repos are queried.
- Links are only opened if they are web links; downloads only from `github.com`.
- Paths in the window show the user folder as `~`, so your Windows user name is not visible on stream.

Where the repo comes from, in this order:
1. your own assignment via "Set repository…"
2. `urls.repository` from the plugin's `manifest.json` (new in OBS 33)
3. the bundled list [`data/known-plugins.json`](data/known-plugins.json)

## OBS 33: what changes for plugin locations

Source: [obsproject/obs-studio#13594](https://github.com/obsproject/obs-studio/pull/13594), [KB "Legacy Plugin Locations"](https://obsproject.com/kb/legacy-plugin-locations).

| System | Current (OBS 33+) | Legacy (loads only until OBS 34) |
|---|---|---|
| macOS | `~/Library/Application Support/obs-studio/plugins/<name>.plugin` | `…/obs-studio/plugins/<name>/bin/<name>.so` (also under `/Library`) |
| Windows | `C:\ProgramData\obs-studio\plugins\<name>\<name>.dll` + `data\` | `…\plugins\<name>\bin\64bit\<name>.dll`, `C:\Program Files\obs-studio\obs-plugins\64bit\` |
| Linux | `~/.local/share/obs-studio/plugins/<name>/<name>.so`, `/usr/lib/obs-modules/plugins/` | `~/.config/obs-studio/plugins/<name>/bin/64bit/`, `/usr/lib/obs-plugins/` |

New locations take precedence. If a plugin exists twice, OBS 33 loads the new copy and blocks the old one.

## Building (macOS)

The Command Line Tools are enough (`xcode-select --install`); Xcode and CMake are not needed. OBS must be installed at `/Applications/OBS.app`, or set `OBS_APP=/path/to/OBS.app`.

```bash
./build-macos.sh install
```

The script downloads the OBS 31.1.1 sources (headers only) and the Qt 6.8.3 headers from `obsproject/obs-deps` once, each with a checksum. These are the same versions [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) uses. It then builds `build/obs-plugin-checker.plugin`, signs it ad hoc and copies it to `~/Library/Application Support/obs-studio/plugins/`. Restart OBS afterwards.

**Compatibility:** The plugin is built against the libobs 31.1 API and therefore loads in OBS 31.1, 32.x and 33. Functions that only exist in OBS 33 (`obs_get_module_version`, `obs_is_legacy_module`) are looked up at runtime and only used if present. The plugin includes an OBS 33 `manifest.json`.

It is built for the architecture of the installed OBS.app (here arm64). A universal build needs a universal OBS to link against.

### Testing without the OBS window

```bash
./build-macos.sh test
```

This starts libobs from OBS.app, loads the plugin via `obs_open_module` (same version and export checks as OBS), runs the scan and the GitHub query and saves an image of the dialog to `build/dialog.png`. `./build-macos.sh test force` bypasses the cache.

## Tested

| | Result |
|---|---|
| OBS 32.1.0 (macOS arm64, Qt 6.8.3) | loads, startup check runs, no errors in the log |
| OBS 33.0.0 (macOS arm64, Qt 6.11.1) | loads, startup check runs, plugin manager shows name/ID/version from `manifest.json` |
| OBS 32.1.0 (macOS arm64), CI universal build | loads without a Gatekeeper warning; versions, latest, status, download file and download are correct; second Stream Deck copy under `/Library/…` detected correctly |
| OBS 32.0.2 (Ubuntu 24.04, Xvfb), CI build | detection, versions and legacy note correct; plugin that doesn't load detected; `-legacy.deb` chosen (replayed GitHub responses); remove and `restore.sh` |
| Headless (`./build-macos.sh test`) | detects update (3.0.2 → 3.2.1), legacy folder, duplicate copies, repo from `manifest.json`, GitHub limit with cache fallback |

**Known OBS 33 bug:** If a third-party plugin has a `manifest.json`, OBS 33.0.0 reports 6 extra "memory leaks" on exit. Without a manifest it's 0. This happens in OBS itself and is harmless.

## Notes

- **GitHub limit:** Without login GitHub allows 60 API requests per hour. Responses are cached for 6 hours; "Check again" queries again. The plugin deliberately does not log in and does not read tokens from the environment.
- **Network:** The plugin uses libcurl instead of Qt Network, because OBS on macOS does not ship a Qt TLS backend. Requests run on their own thread.
- **Downloads:** Downloads open in the browser. Quit OBS before installing.
- **Settings:** Your own assignments and the cache are stored in the OBS folder `plugin_config/obs-plugin-checker/`, i.e. `~/Library/Application Support/obs-studio/` on macOS, `~/.config/obs-studio/` on Linux and `%APPDATA%\obs-studio\` on Windows (portable OBS: in its own `config` folder).
- **Builds:** CI builds Windows x64 (ZIP), macOS universal (`.plugin` as ZIP and `.pkg`) and Ubuntu 24.04 x86_64 (`.tar.xz`) for every pull request. "Update…" is Windows-only so far; on macOS and Linux, "Download" opens the matching installer or package in the browser.

## Project structure

```
src/plugin-main.cpp      module entry, menu item, startup check
src/plugin-scanner.cpp   finding plugins: libobs modules + plugin folders, legacy detection, versions
src/update-checker.cpp   GitHub queries, cache, comparison
src/http-worker.cpp      libcurl on its own thread
src/checker-dialog.cpp   window
data/                    locale (11 languages), known-plugins.json, manifest.json
tools/harness.cpp        headless test
tools/obs_plugin_checker.py  original command-line script
```

## How this project is made 🤖

Full transparency: this project is **vibe-coded**. The maintainer describes what the plugin should do and tests everything in real OBS installations, while the code and translations are written by an AI coding assistant ([Claude Code](https://claude.com/claude-code)). Every change is still built and tested before a release, and the whole history is public in the commits.

If you find something odd, please open an [issue](../../issues). Bug reports are very welcome.

## Disclaimer

Use at your own risk. The Plugin Checker moves and replaces plugin files; it keeps backups and restore scripts, but please back up your OBS folder before updating or removing plugins. The software is provided without warranty; to the extent permitted by law, the authors are not liable for any damage, see sections 15 and 16 of the [GPL](LICENSE.md). This project is not affiliated with or endorsed by the OBS Project or the plugin authors.

## Support

To make macOS open the packages without a warning, they have to be signed and notarized with an Apple Developer account, which costs 99 € a year. The Plugin Checker is free and a hobby project. If it helps you and you'd like to support my work, I'd be really happy, and maybe one day it covers signed packages: [ko-fi.com/n3ckarmann](https://ko-fi.com/n3ckarmann)

## License

GNU General Public License v3.0, see [LICENSE.md](LICENSE.md).
