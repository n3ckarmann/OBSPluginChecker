#!/usr/bin/env python3
"""OBS Plugin-Checker: findet installierte OBS-Plugins und prüft auf GitHub, ob es neuere Versionen gibt.

Nur Python-Standardbibliothek (ab 3.8). Läuft auf macOS, Windows und Linux.

Beispiele:
    python3 obs_plugin_checker.py                       # alle Plugins prüfen
    python3 obs_plugin_checker.py --search              # für unbekannte Plugins Repo-Kandidaten suchen
    python3 obs_plugin_checker.py --map my-plugin user/my-plugin-repo
    python3 obs_plugin_checker.py --download obs-move-transition
    python3 obs_plugin_checker.py --json

Ohne Anmeldung bei GitHub (60 Anfragen/Stunde); es werden keine Tokens gelesen.
"""

import argparse
import json
import os
import platform
import plistlib
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
USER_MAP_FILE = HERE / "obs-plugin-map.json"
CACHE_FILE = Path.home() / ".cache" / "obs-plugin-checker" / "github.json"
CACHE_TTL = 3600
OBS_REPO = "obsproject/obs-studio"

# Bekannte Plugins: Modulname (Ordner-/Bundle-Name) -> GitHub-Repo oder Homepage.
# Eigene Einträge gehören in obs-plugin-map.json (wird per --map gepflegt) und überschreiben diese Liste.
KNOWN_PLUGINS = {
    "StreamDeckPlugin": {"homepage": "https://help.elgato.com/hc/en-us/articles/360028234471",
                         "note": "Elgato, nicht auf GitHub – Update kommt über die Stream-Deck-App"},
    "advanced-scene-switcher": {"repo": "WarmUpTill/SceneSwitcher"},
    "distroav": {"repo": "DistroAV/DistroAV"},
    "obs-ndi": {"repo": "DistroAV/DistroAV", "note": "obs-ndi heißt jetzt DistroAV"},
    "obs-move-transition": {"repo": "exeldro/obs-move-transition"},
    "move-transition": {"repo": "exeldro/obs-move-transition"},
    "obs-shaderfilter": {"repo": "exeldro/obs-shaderfilter"},
    "source-clone": {"repo": "exeldro/obs-source-clone"},
    "source-record": {"repo": "exeldro/obs-source-record"},
    "obs-3d-effect": {"repo": "exeldro/obs-3d-effect"},
    "dir-watch-media": {"repo": "exeldro/obs-dir-watch-media"},
    "downstream-keyer": {"repo": "exeldro/obs-downstream-keyer"},
    "aitum-multistream": {"repo": "Aitum/obs-aitum-multistream"},
    "vertical-canvas": {"repo": "Aitum/obs-vertical-canvas"},
    "obs-composite-blur": {"repo": "FiniteSingularity/obs-composite-blur"},
    "obs-stroke-glow-shadow": {"repo": "FiniteSingularity/obs-stroke-glow-shadow"},
    "obs-advanced-masks": {"repo": "FiniteSingularity/obs-advanced-masks"},
    "obs-backgroundremoval": {"repo": "royshil/obs-backgroundremoval"},
    "obs-localvocal": {"repo": "royshil/obs-localvocal"},
    "obs-multi-rtmp": {"repo": "sorayuki/obs-multi-rtmp"},
    "obs-gstreamer": {"repo": "fzwoch/obs-gstreamer"},
    "waveform": {"repo": "phandasm/waveform"},
    "tuna": {"repo": "univrsal/tuna"},
    "input-overlay": {"repo": "univrsal/input-overlay"},
    "scale-to-sound": {"repo": "dimtpap/obs-scale-to-sound"},
    "StreamFX": {"repo": "Vhonowslend/StreamFX-Public",
                 "note": "StreamFX wird nicht mehr weiterentwickelt – Alternativen suchen"},
}


# ---------------------------------------------------------------- Hilfsfunktionen

def version_tuple(v):
    """'v1.2.3-beta1' -> (1, 2, 3). Leere/unbekannte Versionen -> ()."""
    if not v:
        return ()
    m = re.search(r"\d+(?:\.\d+)*", v)
    return tuple(int(x) for x in m.group(0).split(".")) if m else ()


def is_newer(latest, installed):
    a, b = version_tuple(latest), version_tuple(installed)
    if not a or not b:
        return None
    n = max(len(a), len(b))
    return a + (0,) * (n - len(a)) > b + (0,) * (n - len(b))


def parse_date(s):
    return datetime.strptime(s, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc) if s else None


def load_json(path, default):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return default


def save_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


# ---------------------------------------------------------------- OBS-Installation finden

def obs_config_dir():
    system = platform.system()
    if system == "Darwin":
        return Path.home() / "Library/Application Support/obs-studio"
    if system == "Windows":
        return Path(os.environ.get("APPDATA", "")) / "obs-studio"
    return Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "obs-studio"


def installed_obs_version():
    system = platform.system()
    try:
        if system == "Darwin":
            for app in ("/Applications/OBS.app", str(Path.home() / "Applications/OBS.app")):
                plist = Path(app) / "Contents/Info.plist"
                if plist.exists():
                    with open(plist, "rb") as f:
                        return plistlib.load(f).get("CFBundleShortVersionString")
        elif system == "Windows":
            exe = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "obs-studio/bin/64bit/obs64.exe"
            if exe.exists():
                return windows_file_version(exe)
        else:
            out = subprocess.run(["obs", "--version"], capture_output=True, text=True, timeout=10).stdout
            return version_from_text(out)
    except Exception:
        pass
    # Fallback: Versionszeile im neuesten Log ("OBS 32.1.0 (mac)")
    log = latest_log()
    if log:
        m = re.search(r"\bOBS (\d+\.\d+\.\d+)", log.read_text(errors="replace")[:5000])
        if m:
            return m.group(1)
    return None


def version_from_text(text):
    m = re.search(r"\d+\.\d+(?:\.\d+)?", text or "")
    return m.group(0) if m else None


def windows_file_version(path):
    import ctypes
    from ctypes import wintypes
    ver = ctypes.windll.version
    size = ver.GetFileVersionInfoSizeW(str(path), None)
    if not size:
        return None
    buf = ctypes.create_string_buffer(size)
    ver.GetFileVersionInfoW(str(path), 0, size, buf)
    ptr, length = ctypes.c_void_p(), wintypes.UINT()
    ver.VerQueryValueW(buf, "\\", ctypes.byref(ptr), ctypes.byref(length))
    ms, ls = ctypes.cast(ptr, ctypes.POINTER(wintypes.DWORD * 4)).contents[2:4]
    return "%d.%d.%d" % (ms >> 16, ms & 0xFFFF, ls >> 16)


def latest_log():
    logs = obs_config_dir() / "logs"
    files = sorted(logs.glob("*.txt"), key=lambda p: p.stat().st_mtime, reverse=True) if logs.is_dir() else []
    return files[0] if files else None


# ---------------------------------------------------------------- Plugins finden

def plugin_dirs():
    system = platform.system()
    if system == "Darwin":
        return [obs_config_dir() / "plugins", Path("/Library/Application Support/obs-studio/plugins")]
    if system == "Windows":
        return [Path(os.environ.get("ProgramData", r"C:\ProgramData")) / "obs-studio/plugins",
                obs_config_dir() / "plugins"]
    return [obs_config_dir() / "plugins",
            Path.home() / ".var/app/com.obsproject.Studio/config/obs-studio/plugins"]


def find_plugins():
    found = {}
    for base in plugin_dirs():
        if not base.is_dir():
            continue
        for entry in sorted(base.iterdir()):
            if entry.name.startswith("."):
                continue
            name = entry.name[:-len(".plugin")] if entry.suffix == ".plugin" else entry.name
            if name in found or not (entry.is_dir() or entry.suffix in (".dll", ".so")):
                continue
            found[name] = {"name": name, "path": str(entry), "version": plugin_version(entry, name)}
    return list(found.values())


def plugin_version(entry, name):
    try:
        plist = entry / "Contents/Info.plist"
        if plist.exists():
            with open(plist, "rb") as f:
                info = plistlib.load(f)
            return info.get("CFBundleShortVersionString") or info.get("CFBundleVersion")
        if platform.system() == "Windows":
            for dll in [entry / "bin/64bit" / (name + ".dll"), *entry.glob("bin/64bit/*.dll")]:
                if dll.exists():
                    v = windows_file_version(dll)
                    if v and v != "0.0.0":
                        return v
    except Exception:
        pass
    return None


def plugin_manager_states():
    """OBS 32+ merkt sich im Plugin-Manager, welche Module deaktiviert sind."""
    data = load_json(obs_config_dir() / "plugin_manager/modules.json", [])
    states = {}
    for item in data if isinstance(data, list) else []:
        if isinstance(item, dict):
            name = item.get("module_name") or item.get("name") or item.get("display_name")
            if name is not None and "enabled" in item:
                states[name] = bool(item["enabled"])
    return states


def redact(line):
    """Log lines can contain URLs with tokens or keys (browser sources, docks): never print them."""
    line = re.sub(r"\b[a-z][a-z0-9+.-]*://\S+", "<URL>", line, flags=re.I)
    return re.sub(r"(?i)\b(key|token|secret|password|auth)\w*\s*[=:]\s*\S+", r"\1=<entfernt>", line)


def log_findings(names):
    """Sucht im letzten OBS-Log, ob die Plugins geladen wurden oder Fehler gemeldet haben."""
    log = latest_log()
    if not log:
        return {}, None
    text = log.read_text(errors="replace")
    loaded = set(re.findall(r"obs_init_module\(([^)]+)\)", text))
    m = re.search(r"Loaded Modules:\n((?:.*\n)*?)\S*: -{3,}", text)
    if m:
        loaded |= set(re.findall(r":\s+(\S+?)(?:\.plugin|\.so|\.dll)?\s*$", m.group(1), re.M))
    result = {}
    for name in names:
        errors = [redact(line.split(": ", 1)[-1].strip()) for line in text.splitlines()
                  if name.lower() in line.lower()
                  and re.search(r"fail|error|incompatib|newer libobs|not supported|could not", line, re.I)]
        result[name] = {"loaded": name in loaded, "errors": errors[:3]}
    return result, log


def log_is_current(log, plugin_path):
    """Das Log sagt nur etwas über ein Plugin aus, wenn OBS nach dessen Installation gestartet wurde."""
    try:
        return log.stat().st_mtime > Path(plugin_path).stat().st_mtime
    except OSError:
        return False


# ---------------------------------------------------------------- GitHub

class GitHub:
    def __init__(self, offline=False):
        self.cache = load_json(CACHE_FILE, {})
        self.offline = offline
        self.rate_limited = False

    def get(self, path):
        hit = self.cache.get(path)
        if hit and (self.offline or time.time() - hit["t"] < CACHE_TTL):
            return hit["data"]
        if self.offline or self.rate_limited:
            return None
        req = urllib.request.Request("https://api.github.com" + path, headers={
            "Accept": "application/vnd.github+json", "User-Agent": "obs-plugin-checker"})
        try:
            with urllib.request.urlopen(req, timeout=15) as r:
                data = json.load(r)
        except urllib.error.HTTPError as e:
            if e.code == 403 and e.headers.get("X-RateLimit-Remaining") == "0":
                self.rate_limited = True
                return hit["data"] if hit else None
            data = {"_error": e.code}
        except (urllib.error.URLError, OSError):
            return hit["data"] if hit else None
        self.cache[path] = {"t": time.time(), "data": data}
        return data

    def save(self):
        save_json(CACHE_FILE, self.cache)

    def latest_release(self, repo):
        rel = self.get("/repos/%s/releases/latest" % repo)
        if rel and rel.get("_error") == 404:
            # Manche Repos haben nur Pre-Releases oder Tags.
            rels = self.get("/repos/%s/releases?per_page=5" % repo)
            if isinstance(rels, list) and rels:
                return rels[0]
            tags = self.get("/repos/%s/tags?per_page=1" % repo)
            if isinstance(tags, list) and tags:
                return {"tag_name": tags[0]["name"], "html_url": "https://github.com/%s/tags" % repo, "assets": []}
        return rel

    def repo_info(self, repo):
        return self.get("/repos/" + repo)

    def search(self, name):
        q = urllib.request.quote("%s obs in:name,description" % name)
        res = self.get("/search/repositories?per_page=5&q=" + q)
        return [i["full_name"] for i in (res or {}).get("items", [])]


def pick_asset(assets):
    """Wählt den Download, der zum Betriebssystem passt."""
    system = platform.system()
    if system == "Darwin":
        prefs = [r"mac.*\.pkg$", r"\.pkg$", r"mac.*\.(zip|tar\.xz|dmg)$"]
    elif system == "Windows":
        prefs = [r"win.*\.exe$", r"win.*\.zip$", r"\.exe$", r"windows.*\.zip$"]
    else:
        prefs = [r"(ubuntu|linux).*\.deb$", r"\.deb$", r"linux.*\.(tar\.xz|zip)$", r"\.flatpak$"]
    for pattern in prefs:
        for a in assets or []:
            if re.search(pattern, a["name"], re.I) and not a["name"].endswith((".sig", ".sha256")):
                return a
    return None


# ---------------------------------------------------------------- Auswertung

def check(gh, obs_version):
    user_map = load_json(USER_MAP_FILE, {})
    states = plugin_manager_states()
    plugins = find_plugins()
    logs, log_path = log_findings([p["name"] for p in plugins])

    obs_latest = gh.latest_release(OBS_REPO) or {}
    obs_latest_tag = obs_latest.get("tag_name")
    obs_major_date = None
    if obs_latest_tag:
        # Release-Datum der aktuellen Hauptversion (z. B. 33.0.0) – Plugins, die seitdem kein Release hatten, sind verdächtig.
        major = version_tuple(obs_latest_tag)[:1]
        obs_major_date = parse_date(obs_latest.get("published_at"))
        if version_tuple(obs_latest_tag)[1:] != (0, 0):
            rels = gh.get("/repos/%s/releases?per_page=30" % OBS_REPO) or []
            for r in rels if isinstance(rels, list) else []:
                if version_tuple(r["tag_name"])[:1] == major and not r.get("prerelease"):
                    obs_major_date = parse_date(r["published_at"])

    results = []
    for p in plugins:
        entry = dict(KNOWN_PLUGINS.get(p["name"], {}), **user_map.get(p["name"], {}))
        current = log_path is not None and log_is_current(log_path, p["path"])
        r = dict(p, repo=entry.get("repo"), homepage=entry.get("homepage"), note=entry.get("note"),
                 enabled=states.get(p["name"]), log=logs.get(p["name"]) if current else None,
                 log_current=current, status="unknown")
        if r["repo"]:
            rel = gh.latest_release(r["repo"])
            info = gh.repo_info(r["repo"]) or {}
            if not rel or rel.get("_error"):
                r["status"] = "error"
                r["note"] = "Repo nicht gefunden oder GitHub nicht erreichbar"
            else:
                r["latest"] = rel.get("tag_name")
                r["released"] = rel.get("published_at")
                r["release_url"] = rel.get("html_url")
                asset = pick_asset(rel.get("assets"))
                r["download"] = asset and asset["browser_download_url"]
                r["archived"] = bool(info.get("archived"))
                newer = is_newer(r["latest"], p["version"])
                r["status"] = "update" if newer else "ok" if newer is False else "check"
                pub = parse_date(r["released"])
                r["before_obs_major"] = bool(obs_major_date and pub and pub < obs_major_date)
        elif r["homepage"]:
            r["status"] = "manual"
        results.append(r)
    return {"obs_installed": obs_version, "obs_latest": obs_latest_tag, "log": str(log_path) if log_path else None,
            "plugins": results}


# ---------------------------------------------------------------- Ausgabe

COLORS = sys.stdout.isatty() and os.environ.get("NO_COLOR") is None


def c(text, code):
    return "\033[%sm%s\033[0m" % (code, text) if COLORS else text


STATUS_LABEL = {
    "update": ("UPDATE", "1;33"), "ok": ("aktuell", "32"), "check": ("prüfen", "36"),
    "manual": ("manuell", "36"), "unknown": ("unbekannt", "90"), "error": ("Fehler", "31"),
}


def print_report(report):
    inst, latest = report["obs_installed"], report["obs_latest"]
    line = "OBS installiert: %s   neueste Version: %s" % (inst or "?", latest or "?")
    if is_newer(latest, inst):
        line += "   " + c("→ OBS-Update verfügbar", "1;33")
    print(line)
    if report["log"]:
        print(c("Letztes Log: " + report["log"], "90"))
    print()
    if not report["plugins"]:
        print("Keine Drittanbieter-Plugins gefunden in:")
        for d in plugin_dirs():
            print("  ", d)
        return

    for r in report["plugins"]:
        label, color = STATUS_LABEL[r["status"]]
        print("%s  %s" % (c("%-10s" % label, color), c(r["name"], "1")))
        print("            installiert: %s" % (r["version"] or "Version nicht ermittelbar"), end="")
        if r.get("latest"):
            print("   neueste: %s (%s)" % (r["latest"], (r.get("released") or "")[:10]), end="")
        print()
        if r["repo"]:
            print("            https://github.com/" + r["repo"])
        if r["homepage"]:
            print("            " + r["homepage"])
        if r["status"] == "update" and r.get("download"):
            print("            Download: " + r["download"])
        elif r.get("release_url") and r["status"] in ("update", "check"):
            print("            Release:  " + r["release_url"])

        hints = []
        if r["enabled"] is False:
            hints.append("im Plugin-Manager deaktiviert")
        elif r["log"] and not r["log"]["loaded"]:
            hints.append("wurde beim letzten OBS-Start NICHT geladen (inkompatibel oder deaktiviert?)")
        elif not r["log_current"]:
            hints.append("seit der Installation kein OBS-Log – ob es lädt, zeigt erst der nächste OBS-Start")
        for e in r["log"].get("errors", []) if r["log"] else []:
            hints.append("Log: " + e)
        if r.get("archived"):
            hints.append("Repo ist archiviert – wird nicht mehr gepflegt")
        if r.get("before_obs_major") and is_newer(latest, inst) is not None:
            hints.append("letztes Release ist älter als OBS %s – Kompatibilität prüfen"
                         % ".".join(map(str, version_tuple(latest)[:1])))
        if r["status"] == "unknown":
            hints.append("kein Repo bekannt – mit --search suchen oder per --map NAME owner/repo eintragen")
        if r.get("note"):
            hints.append(r["note"])
        for h in hints:
            print("            " + c("• " + h, "33"))
        print()


# ---------------------------------------------------------------- Aktionen

def do_map(name, target):
    data = load_json(USER_MAP_FILE, {})
    if target.startswith("http"):
        data[name] = {"homepage": target}
    else:
        target = re.sub(r"^https?://github\.com/|\.git$|/$", "", target)
        data[name] = {"repo": target}
    save_json(USER_MAP_FILE, data)
    print("%s → %s gespeichert in %s" % (name, target, USER_MAP_FILE))


def do_search(gh, report):
    for r in report["plugins"]:
        if r["status"] != "unknown":
            continue
        hits = gh.search(r["name"])
        print("%s:" % r["name"])
        for h in hits or ["(nichts gefunden)"]:
            print("   ", h)
        if hits:
            print("    → übernehmen mit: python3 %s --map %s %s" % (Path(__file__).name, r["name"], hits[0]))


def do_download(report, name):
    r = next((r for r in report["plugins"] if r["name"] == name), None)
    if not r:
        sys.exit("Plugin '%s' nicht gefunden." % name)
    if not r.get("download"):
        sys.exit("Kein passender Download für dieses System – siehe %s" % (r.get("release_url") or r.get("homepage")))
    target = Path.home() / "Downloads" / r["download"].rsplit("/", 1)[-1]
    print("Lade %s → %s" % (r["download"], target))
    req = urllib.request.Request(r["download"], headers={"User-Agent": "obs-plugin-checker"})
    with urllib.request.urlopen(req, timeout=120) as resp, open(target, "wb") as f:
        while True:
            chunk = resp.read(1 << 16)
            if not chunk:
                break
            f.write(chunk)
    print("Fertig. OBS beenden, dann die Datei öffnen/installieren.")


def main():
    ap = argparse.ArgumentParser(description="Prüft installierte OBS-Plugins auf neue Versionen (GitHub).")
    ap.add_argument("--json", action="store_true", help="Ergebnis als JSON ausgeben")
    ap.add_argument("--offline", action="store_true", help="nur Cache verwenden, keine Netzwerkanfragen")
    ap.add_argument("--search", action="store_true", help="GitHub-Repos für unbekannte Plugins vorschlagen")
    ap.add_argument("--map", nargs=2, metavar=("NAME", "REPO_ODER_URL"), help="Plugin einem Repo/einer Seite zuordnen")
    ap.add_argument("--download", metavar="NAME", help="neueste Version eines Plugins nach ~/Downloads laden")
    args = ap.parse_args()

    if args.map:
        return do_map(*args.map)

    gh = GitHub(offline=args.offline)
    report = check(gh, installed_obs_version())
    gh.save()

    if args.json:
        print(json.dumps(report, indent=2, ensure_ascii=False))
    elif args.search:
        do_search(gh, report)
        gh.save()
    elif args.download:
        do_download(report, args.download)
    else:
        print_report(report)
    if gh.rate_limited:
        print(c("Hinweis: GitHub-Limit erreicht – später erneut versuchen.", "31"),
              file=sys.stderr)


if __name__ == "__main__":
    main()
