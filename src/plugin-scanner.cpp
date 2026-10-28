#include "plugin-scanner.hpp"
#include "pe-file.hpp"
#include "plugin-usage.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define PLUGIN_SELF "obs-plugin-checker"

/* Modules shipped with OBS itself (OBS 31-33). OBS 33 can tell us directly, older versions cannot. */
static const QSet<QString> kCoreModules = {
	"aja",           "aja-output-ui",    "coreaudio-encoder", "decklink",       "decklink-captions",
	"decklink-output-ui", "frontend-tools", "image-source",  "linux-alsa",     "linux-capture",
	"linux-jack",    "linux-pipewire",   "linux-pulseaudio",  "linux-v4l2",     "mac-avcapture",
	"mac-avcapture-legacy", "mac-capture", "mac-syphon",     "mac-videotoolbox", "mac-virtualcam",
	"nv-filters",    "obs-browser",      "obs-ffmpeg",        "obs-filters",    "obs-libfdk",
	"obs-nvenc",     "obs-outputs",      "obs-qsv11",         "obs-text",       "obs-transitions",
	"obs-vst",       "obs-webrtc",       "obs-websocket",     "obs-x264",       "oss-audio",
	"rtmp-services", "sndio",            "text-freetype2",    "vlc-video",      "win-capture",
	"win-dshow",     "win-wasapi",       "win-mf",            "enc-amf",        "obs-qsv"};

/* Data folders that older OBS versions left behind, not from third-party plugins. */
static const QSet<QString> kCoreLeftovers = {"decklink-ouput-ui", "obs-browser-page", "win-decklink", "win-mf"};

/* ------------------------------------------------------------------ OBS 33 APIs, looked up at runtime */

static void *find_obs_symbol(const char *name)
{
#ifdef _WIN32
	HMODULE obs = GetModuleHandleW(L"obs.dll");
	return obs ? reinterpret_cast<void *>(GetProcAddress(obs, name)) : nullptr;
#else
	return dlsym(RTLD_DEFAULT, name);
#endif
}

using module_str_fn = const char *(*)(obs_module_t *);
using module_bool_fn = bool (*)(obs_module_t *);

static const char *call_str(const char *symbol, obs_module_t *mod)
{
	auto fn = reinterpret_cast<module_str_fn>(find_obs_symbol(symbol));
	return fn ? fn(mod) : nullptr;
}

static int call_bool(const char *symbol, obs_module_t *mod)
{
	auto fn = reinterpret_cast<module_bool_fn>(find_obs_symbol(symbol));
	return fn ? (fn(mod) ? 1 : 0) : -1;
}

/* ------------------------------------------------------------------ helpers */

static QString version_digits(const QString &v)
{
	static const QRegularExpression re("\\d+(?:\\.\\d+)*");
	auto m = re.match(v);
	return m.hasMatch() ? m.captured(0) : QString();
}

bool has_version_number(const QString &v)
{
	return !version_digits(v).isEmpty();
}

int compare_versions(const QString &a, const QString &b)
{
	QStringList pa = version_digits(a).split('.', Qt::SkipEmptyParts);
	QStringList pb = version_digits(b).split('.', Qt::SkipEmptyParts);
	if (pa.isEmpty() || pb.isEmpty())
		return 0;
	for (int i = 0; i < qMax(pa.size(), pb.size()); i++) {
		qlonglong x = i < pa.size() ? pa[i].toLongLong() : 0;
		qlonglong y = i < pb.size() ? pb[i].toLongLong() : 0;
		if (x != y)
			return x < y ? -1 : 1;
	}
	return 0;
}

QString obs_user_config_dir()
{
	/* <config>/obs-studio/plugin_config/<module>/ -> <config>/obs-studio. Unlike os_get_config_path,
	 * this follows the frontend's config location, so portable mode is handled. */
	if (char *moduleConfig = obs_module_config_path("")) {
		/* relative in portable mode (../../config/...), resolved like libobs does: from the working directory */
		QString dir = QDir::cleanPath(QFileInfo(QString::fromUtf8(moduleConfig) + "/../..").absoluteFilePath());
		bfree(moduleConfig);
		if (QFileInfo(dir).isDir())
			return dir;
	}
	char *path = os_get_config_path_ptr("obs-studio");
	QString result = QString::fromUtf8(path);
	bfree(path);
	return result;
}

static bool is_inside(const QString &path, const QString &dir)
{
	if (dir.isEmpty())
		return false;
	QString d = QDir::cleanPath(dir) + "/";
	return QDir::cleanPath(path).startsWith(d, Qt::CaseInsensitive);
}

#ifdef _WIN32
/* <install>/bin/64bit/obs64.exe -> <install> */
QString obs_install_root()
{
	return QDir(QCoreApplication::applicationDirPath() + "/../..").absolutePath();
}
#endif

/* A portable OBS keeps everything in its own folder. It never loads from ProgramData and its
 * settings are not in AppData, so the checker must not look at or touch those. */
bool obs_is_portable()
{
#ifdef _WIN32
	if (char *moduleConfig = obs_module_config_path("")) {
		bool relative = QDir::isRelativePath(QString::fromUtf8(moduleConfig));
		bfree(moduleConfig);
		if (relative)
			return true;
	}
	for (const char *marker : {"portable_mode", "obs_portable_mode", "portable_mode.txt", "obs_portable_mode.txt"})
		if (QFileInfo::exists(obs_install_root() + "/" + marker))
			return true;
	return is_inside(obs_user_config_dir(), obs_install_root());
#else
	return false;
#endif
}

QStringList removable_roots()
{
	QStringList roots = {obs_user_config_dir() + "/plugin_config"};
#if defined(__APPLE__)
	roots << QDir::homePath() + "/Library/Application Support/obs-studio/plugins"
	      << "/Library/Application Support/obs-studio/plugins";
#elif defined(_WIN32)
	if (obs_is_portable())
		return {obs_install_root()};
	roots << obs_install_root() << qEnvironmentVariable("ProgramData", "C:/ProgramData") + "/obs-studio/plugins";
#else
	roots << qEnvironmentVariable("XDG_DATA_HOME", QDir::homePath() + "/.local/share") + "/obs-studio/plugins"
	      << qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config") + "/obs-studio/plugins";
#endif
	return roots;
}

static QJsonObject read_json_object(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return {};
	return QJsonDocument::fromJson(f.readAll()).object();
}

static QString repo_from_url(const QString &url)
{
	static const QRegularExpression re("^https?://(?:www\\.)?github\\.com/([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+?)(?:\\.git)?/?(?:[#?].*)?$");
	auto m = re.match(url);
	if (!m.hasMatch())
		return {};
	return m.captured(1);
}

/* CFBundleShortVersionString is the marketing version and should win. Some plugins built from older
 * templates swap the two keys (obs-composite-blur 1.5.2: short "1", bundle "1.5.2"), so a dotted
 * CFBundleVersion that continues a dot-less short version is taken instead. A build number
 * ("42", or "5.5.4.123" next to "5.5.4") never replaces a real short version. */
QString pick_bundle_version(const QString &shortVersion, const QString &bundleVersion)
{
	static const QRegularExpression dotted("^v?\\d+(\\.\\d+)+$");
	if (shortVersion.isEmpty())
		return dotted.match(bundleVersion).hasMatch() ? bundleVersion : QString();
	if (!shortVersion.contains('.') && dotted.match(bundleVersion).hasMatch() &&
	    (bundleVersion.startsWith(shortVersion + '.') || bundleVersion.startsWith('v' + shortVersion + '.')))
		return bundleVersion;
	return shortVersion;
}

static QString bundle_version(const QString &bundlePath)
{
#ifdef __APPLE__
	QByteArray utf8 = bundlePath.toUtf8();
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)utf8.constData(),
							       utf8.size(), true);
	if (!url)
		return {};
	CFBundleRef bundle = CFBundleCreate(nullptr, url);
	CFRelease(url);
	if (!bundle)
		return {};
	auto read = [bundle](CFStringRef key) -> QString {
		auto value = (CFStringRef)CFBundleGetValueForInfoDictionaryKey(bundle, key);
		if (!value || CFGetTypeID(value) != CFStringGetTypeID())
			return {};
		char buf[128];
		if (!CFStringGetCString(value, buf, sizeof(buf), kCFStringEncodingUTF8))
			return {};
		return QString::fromUtf8(buf).trimmed();
	};
	const QString shortVersion = read(CFSTR("CFBundleShortVersionString"));
	const QString bundleVersion = read(CFSTR("CFBundleVersion"));
	CFRelease(bundle);
	return pick_bundle_version(shortVersion, bundleVersion);
#else
	(void)bundlePath;
	return {};
#endif
}

/* Most plugins pass their version as a string literal to blog() ("loaded version %s"), so a single
 * version-like string in the binary is taken as the version. Used where no version resource exists. */
[[maybe_unused]] static QString version_from_strings(const QString &binary)
{
	QFile f(binary);
	if (f.size() > 64 * 1024 * 1024 || !f.open(QIODevice::ReadOnly))
		return {};
	const QByteArray data = f.readAll();
	static const QRegularExpression re("^v?(\\d{1,4}\\.\\d{1,4}\\.\\d{1,6})$");
	QSet<QString> found;
	qsizetype start = 0;
	for (qsizetype i = 0; i < data.size(); i++) {
		if (data[i] != '\0')
			continue;
		qsizetype len = i - start;
		if (len >= 5 && len <= 16) {
			auto m = re.match(QString::fromLatin1(data.constData() + start, len));
			if (m.hasMatch())
				found.insert(m.captured(1));
		}
		start = i + 1;
	}
	return found.size() == 1 ? *found.begin() : QString();
}

static QString file_version(const QString &binary)
{
#ifdef _WIN32
	/* Some plugins (e.g. Source Profiler) ship without a version resource. */
	std::wstring path = binary.toStdWString();
	DWORD size = GetFileVersionInfoSizeW(path.c_str(), nullptr);
	if (!size)
		return version_from_strings(binary);
	QByteArray buf(size, 0);
	if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data()))
		return version_from_strings(binary);
	VS_FIXEDFILEINFO *info = nullptr;
	UINT len = 0;
	if (!VerQueryValueW(buf.data(), L"\\", (void **)&info, &len) || !info)
		return version_from_strings(binary);
	QString v = QString("%1.%2.%3")
			    .arg(HIWORD(info->dwFileVersionMS))
			    .arg(LOWORD(info->dwFileVersionMS))
			    .arg(HIWORD(info->dwFileVersionLS));
	return v == "0.0.0" ? version_from_strings(binary) : v;
#elif defined(__APPLE__)
	(void)binary;
	return {};
#else
	/* ELF has no version resource. */
	return version_from_strings(binary);
#endif
}

/* ------------------------------------------------------------------ plugin folders on disk */

struct Candidate {
	QString name;
	QString location;
	QString binary;
	QString dataPath;
	bool legacy = false;
};

static void scan_folder_layout(QList<Candidate> &out, const QString &base, const QString &binExt, bool layoutIsNew)
{
	QDir dir(base);
	for (const QFileInfo &fi : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		QString name = fi.fileName();
		QString path = fi.absoluteFilePath();
#ifdef __APPLE__
		if (name.endsWith(".plugin")) {
			name.chop(7);
			out.append({name, path, path + "/Contents/MacOS/" + name, path + "/Contents/Resources", false});
			continue;
		}
#endif
		Candidate c{name, path, {}, path + "/data", true};
		QStringList legacyBins = {path + "/bin/64bit/" + name + binExt, path + "/bin/" + name + binExt};
		if (layoutIsNew && QFile::exists(path + "/" + name + binExt)) {
			c.binary = path + "/" + name + binExt;
			c.legacy = false;
		} else {
			for (const QString &b : legacyBins)
				if (QFile::exists(b))
					c.binary = b;
		}
		if (!c.binary.isEmpty())
			out.append(c);
	}
}

#ifndef __APPLE__
static void scan_flat_binaries(QList<Candidate> &out, const QString &binDir, const QString &dataDir,
			       const QString &binExt, bool legacy)
{
	QDir dir(binDir);
	for (const QFileInfo &fi : dir.entryInfoList({"*" + binExt}, QDir::Files)) {
		QString name = fi.completeBaseName();
		if (kCoreModules.contains(name))
			continue;
		/* Flat folders also hold helper libraries (libcef.dll, chrome_elf.dll, ...): only real modules count. */
		if (binExt == ".dll" && !read_pe(fi.absoluteFilePath()).exports.contains("obs_module_load"))
			continue;
		out.append({name, fi.absoluteFilePath(), fi.absoluteFilePath(), dataDir + "/" + name, legacy});
	}
}
#endif

#ifdef _WIN32
/* <install>/bin/64bit/obs64.exe -> <install>/obs-plugins/64bit */
static QString legacy_install_bin_dir()
{
	return QDir(QCoreApplication::applicationDirPath() + "/../../obs-plugins/64bit").absolutePath();
}
#endif

static QList<Candidate> scan_directories()
{
	QList<Candidate> out;
#if defined(__APPLE__)
	scan_folder_layout(out, QDir::homePath() + "/Library/Application Support/obs-studio/plugins", ".so", false);
	scan_folder_layout(out, "/Library/Application Support/obs-studio/plugins", ".so", false);
#elif defined(_WIN32)
	/* A portable OBS keeps its config inside the install folder and ignores ProgramData. */
	QString root = obs_install_root();
	if (!obs_is_portable()) {
		QString programData = qEnvironmentVariable("ProgramData", "C:/ProgramData");
		scan_folder_layout(out, programData + "/obs-studio/plugins", ".dll", true);
	} else {
		/* OBS 33+ portable: <install>/plugins/<name>/<name>.dll */
		scan_folder_layout(out, root + "/plugins", ".dll", true);
	}
	QDir install(QCoreApplication::applicationDirPath() + "/../..");
	scan_flat_binaries(out, legacy_install_bin_dir(), install.absoluteFilePath("data/obs-plugins"), ".dll", true);
#else
	QString dataHome = qEnvironmentVariable("XDG_DATA_HOME", QDir::homePath() + "/.local/share");
	QString configHome = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config");
	scan_folder_layout(out, dataHome + "/obs-studio/plugins", ".so", true);
	scan_folder_layout(out, configHome + "/obs-studio/plugins", ".so", false);
	for (const QString &lib : {"/usr/lib", "/usr/lib64", "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu"}) {
		scan_flat_binaries(out, lib + "/obs-modules/plugins", "/usr/share/obs/obs-modules/plugins", ".so", false);
		scan_flat_binaries(out, lib + "/obs-plugins", "/usr/share/obs/obs-plugins", ".so", true);
	}
#endif
	return out;
}

/* ------------------------------------------------------------------ loaded modules */

struct LoadedModule {
	QString name;
	QString displayName;
	QString binary;
	QString dataPath;
	QString version; // OBS 33+: from manifest.json via libobs
	int legacy = -1; // OBS 33+: 0/1, older: -1
	int core = -1;
};

static QString absolute(const QString &path)
{
	return path.isEmpty() ? path : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

static void enum_module(void *param, obs_module_t *mod)
{
	auto list = static_cast<QList<LoadedModule> *>(param);
	LoadedModule m;
	m.name = QString::fromUtf8(obs_get_module_file_name(mod));
	m.displayName = QString::fromUtf8(obs_get_module_name(mod));
	/* libobs keeps these relative to the working directory (../../obs-plugins/64bit/...) */
	m.binary = absolute(QString::fromUtf8(obs_get_module_binary_path(mod)));
	m.dataPath = absolute(QString::fromUtf8(obs_get_module_data_path(mod)));
	m.version = QString::fromUtf8(call_str("obs_get_module_version", mod));
	m.legacy = call_bool("obs_is_legacy_module", mod);
	m.core = call_bool("obs_is_core_module", mod);
	/* obs_get_module_file_name can contain the extension */
	for (const char *ext : {".so", ".dll", ".dylib"})
		if (m.name.endsWith(ext))
			m.name.chop(int(strlen(ext)));
	list->append(m);
}

static bool is_core(const LoadedModule &m)
{
	if (m.core == 1)
		return true;
	if (m.core == 0)
		return false;
	return kCoreModules.contains(m.name) || m.binary.contains(".app/Contents/PlugIns/");
}

static QHash<QString, bool> plugin_manager_states()
{
	QHash<QString, bool> states;
	QFile f(obs_user_config_dir() + "/plugin_manager/modules.json");
	if (!f.open(QIODevice::ReadOnly))
		return states;
	for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).array()) {
		QJsonObject o = v.toObject();
		if (o.contains("module_name") && o.contains("enabled"))
			states.insert(o["module_name"].toString(), o["enabled"].toBool());
	}
	return states;
}

static bool same_file(const QString &a, const QString &b)
{
	return !a.isEmpty() && !b.isEmpty() && QFileInfo(a).canonicalFilePath() == QFileInfo(b).canonicalFilePath();
}

/* ------------------------------------------------------------------ files that belong to a plugin */

static void add_tree(QStringList &files, const QString &path)
{
	QFileInfo fi(path);
	if (path.isEmpty() || !fi.exists())
		return;
	if (!fi.isDir()) {
		files.append(fi.absoluteFilePath());
		return;
	}
	QDirIterator it(fi.absoluteFilePath(), QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
	while (it.hasNext())
		files.append(it.next());
}

struct HelperLibraries {
	QHash<QString, QStringList> byModule; // lower-case module file name -> helper library paths
	QHash<QString, int> users;            // helper library path -> number of modules that need it
};

#ifdef _WIN32
/* Helper DLLs in a flat plugin folder that are not modules themselves (libcef.dll for obs-browser,
 * an SDK runtime for a capture plugin, ...). Each one is assigned to every module that imports it,
 * directly or through other helper DLLs in the same folder. */
static HelperLibraries scan_helper_libraries(const QString &binDir)
{
	HelperLibraries result;
	QHash<QString, QString> helpers;      // lower-case name -> path
	QHash<QString, QStringList> imports;  // lower-case name -> lower-case imported names
	QStringList modules;
	for (const QFileInfo &fi : QDir(binDir).entryInfoList({"*.dll"}, QDir::Files)) {
		PeInfo pe = read_pe(fi.absoluteFilePath());
		QString lname = fi.fileName().toLower();
		for (const QString &imp : pe.imports)
			imports[lname].append(imp.toLower());
		if (pe.exports.contains("obs_module_load"))
			modules.append(lname);
		else
			helpers.insert(lname, fi.absoluteFilePath());
	}
	for (const QString &module : modules) {
		QSet<QString> seen;
		QStringList queue = imports.value(module);
		while (!queue.isEmpty()) {
			QString dep = queue.takeFirst();
			if (seen.contains(dep) || !helpers.contains(dep))
				continue;
			seen.insert(dep);
			queue.append(imports.value(dep));
			result.byModule[module].append(helpers[dep]);
			result.users[helpers[dep]]++;
		}
	}
	return result;
}
#endif

#ifdef _WIN32
/* DLLs the plugin imports that Windows will not find: it then fails to load, typically after an OBS
 * update replaced a library (avcodec-61.dll -> avcodec-62.dll). */
static QStringList missing_libraries(const QString &binary)
{
	QStringList dirs = {QFileInfo(binary).absolutePath(), QCoreApplication::applicationDirPath()};
	wchar_t buf[MAX_PATH];
	if (UINT n = GetSystemDirectoryW(buf, MAX_PATH); n && n < MAX_PATH)
		dirs << QString::fromWCharArray(buf, int(n));
	if (UINT n = GetWindowsDirectoryW(buf, MAX_PATH); n && n < MAX_PATH)
		dirs << QString::fromWCharArray(buf, int(n));

	QStringList missing;
	for (const QString &lib : read_pe(binary).imports) {
		if (lib.startsWith("api-ms-win-", Qt::CaseInsensitive) || lib.startsWith("ext-ms-", Qt::CaseInsensitive))
			continue;
		bool found = false;
		for (const QString &dir : dirs)
			found = found || QFileInfo::exists(dir + "/" + lib);
		if (!found)
			missing.append(lib);
	}
	return missing;
}

static QString module_binary(const PluginEntry &e)
{
	QFileInfo loc(e.location);
	if (!loc.isDir())
		return loc.exists() ? loc.absoluteFilePath() : QString();
	for (const QString &candidate : {e.location + "/" + e.name + ".dll", e.location + "/bin/64bit/" + e.name + ".dll",
					 e.location + "/bin/" + e.name + ".dll"})
		if (QFileInfo::exists(candidate))
			return candidate;
	return {};
}
#endif

static void collect_files(PluginEntry &e, const HelperLibraries &helpers, const QString &configDir)
{
	QStringList files;
	QFileInfo loc(e.location);
	if (loc.isDir()) {
		/* plugin folder or macOS bundle: everything inside belongs to it */
		add_tree(files, e.location);
	} else if (loc.exists()) {
		add_tree(files, e.location);
		add_tree(files, loc.absolutePath() + "/" + loc.completeBaseName() + ".pdb");
		for (const QString &helper : helpers.byModule.value(loc.fileName().toLower())) {
			files.append(helper);
			if (helpers.users.value(helper) > 1)
				e.sharedFiles.append(helper);
		}
#ifdef _WIN32
		/* some installers also drop a copy next to obs64.exe */
		QString appDir = QCoreApplication::applicationDirPath();
		if (!is_inside(e.location, appDir)) {
			add_tree(files, appDir + "/" + loc.fileName());
			add_tree(files, appDir + "/" + loc.completeBaseName() + ".pdb");
		}
#endif
	}
	if (!is_inside(e.dataPath, e.location))
		add_tree(files, e.dataPath);
	add_tree(files, configDir + "/plugin_config/" + e.name);
	files.removeDuplicates();
	e.files = files;
}

/* ------------------------------------------------------------------ public */

QList<PluginEntry> scan_plugins()
{
	QList<LoadedModule> loaded;
	obs_enum_modules(enum_module, &loaded);

	QList<Candidate> onDisk = scan_directories();
	QHash<QString, bool> states = plugin_manager_states();

	QList<PluginEntry> result;
	QHash<QString, int> index;

	auto entry_for = [&](const QString &name) -> PluginEntry & {
		if (!index.contains(name)) {
			index.insert(name, int(result.size()));
			PluginEntry e;
			e.name = name;
			result.append(e);
		}
		return result[index[name]];
	};

	QHash<QString, QString> binaries;
	for (const LoadedModule &m : loaded) {
		if (m.name == PLUGIN_SELF || is_core(m))
			continue;
		PluginEntry &e = entry_for(m.name);
		binaries.insert(m.name, m.binary);
		e.loaded = true;
		e.displayName = m.displayName;
		e.location = m.binary;
		e.dataPath = m.dataPath;
		e.version = m.version;
		e.legacy = m.legacy == 1;
		for (const Candidate &c : onDisk) {
			if (c.name == m.name && same_file(c.binary, m.binary)) {
				e.location = c.location;
				if (m.legacy < 0)
					e.legacy = c.legacy;
			}
		}
	}

	for (const Candidate &c : onDisk) {
		if (c.name == PLUGIN_SELF)
			continue;
		bool known = index.contains(c.name);
		PluginEntry &e = entry_for(c.name);
		if (!known) {
			binaries.insert(c.name, c.binary);
			e.location = c.location;
			e.dataPath = c.dataPath;
			e.legacy = c.legacy;
		} else if (e.location != c.location && !same_file(e.location, c.location)) {
			e.duplicates.append(c.location + (c.legacy ? " (Legacy)" : ""));
		}
	}

	for (PluginEntry &e : result) {
		if (states.contains(e.name))
			e.disabled = !states[e.name];

		QJsonObject manifest = read_json_object(e.dataPath + "/manifest.json");
		if (e.version.isEmpty())
			e.version = manifest["version"].toString();
		if (e.version.isEmpty() && e.location.endsWith(".plugin"))
			e.version = bundle_version(e.location);
		if (e.version.isEmpty() && !e.location.endsWith(".plugin"))
			e.version = file_version(binaries.value(e.name));
		if (e.displayName.isEmpty())
			e.displayName = manifest["display_name"].toString();
		e.repo = repo_from_url(manifest["urls"].toObject()["repository"].toString());
		e.homepage = manifest["urls"].toObject()["website"].toString();
	}

	HelperLibraries helpers;
#ifdef _WIN32
	helpers = scan_helper_libraries(legacy_install_bin_dir());

	/* data/obs-plugins/<name> without a module: leftovers of a removed or renamed plugin */
	QDir legacyData(QCoreApplication::applicationDirPath() + "/../../data/obs-plugins");
	for (const QFileInfo &fi : legacyData.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		QString name = fi.fileName();
		if (index.contains(name) || name == PLUGIN_SELF || kCoreModules.contains(name) ||
		    kCoreLeftovers.contains(name))
			continue;
		PluginEntry &e = entry_for(name);
		e.location = fi.absoluteFilePath();
		e.dataPath = fi.absoluteFilePath();
		e.leftover = true;
	}

	for (PluginEntry &e : result)
		if (QString binary = module_binary(e); !binary.isEmpty())
			e.missingLibraries = missing_libraries(binary);
#endif
	QString configDir = obs_user_config_dir();
	QHash<QString, QStringList> types = module_types(configDir);
	QHash<QString, QHash<QString, int>> usage = type_usage(configDir);
	for (PluginEntry &e : result) {
		collect_files(e, helpers, configDir);
		e.types = types.value(e.name);
		for (const QString &type : e.types)
			for (auto it = usage[type].begin(); it != usage[type].end(); ++it)
				e.usage[it.key()] += it.value();
	}

	std::sort(result.begin(), result.end(), [](const PluginEntry &a, const PluginEntry &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
	return result;
}

