#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>


struct PluginEntry {
	enum class Status { Unknown, Checking, UpToDate, Update, Check, Manual, Error };

	// From disk / libobs
	QString name;        // module file name, e.g. "obs-move-transition"
	QString displayName; // obs_module_name() or manifest display_name
	QString location;    // bundle, folder or binary on disk
	QString dataPath;
	QString version;
	bool loaded = false;
	bool disabled = false; // switched off in the plugin manager
	bool legacy = false;   // old folder layout, OBS 34 stops loading these
	QStringList duplicates;
	QStringList files;       // everything on disk that belongs to the plugin, incl. its settings folder
	QStringList sharedFiles; // helper libraries other plugins need as well
	QStringList missingLibraries; // imported DLLs that are not there, so the plugin cannot load
	bool leftover = false;        // only a data folder is left, the plugin itself is gone
	QStringList types;            // source/filter/transition/output/encoder/service ids it provides
	QHash<QString, int> usage;    // scene collection or "profile:<name>" -> number of uses

	// From known-plugins.json, repo-map.json or manifest.json
	QString repo; // owner/name on GitHub
	QString homepage;
	QString note; // locale key or plain text

	// From GitHub
	Status status = Status::Unknown;
	QString latest;
	QString publishedAt;
	QString releaseUrl;
	QString downloadUrl;
	QString downloadDigest; // "sha256:<hex>" from GitHub, empty for older uploads
	bool downloadIsInstaller = false; // .exe/.pkg/.deb: installs into the standard location
	QString error;
	bool archived = false;
	bool releaseBeforeObsMajor = false;
};

QList<PluginEntry> scan_plugins();

/* Returns <0, 0, >0 like strcmp, or 0 when either side has no number. */
int compare_versions(const QString &a, const QString &b);
bool has_version_number(const QString &v);

/* macOS bundles: chooses between CFBundleShortVersionString and CFBundleVersion. */
QString pick_bundle_version(const QString &shortVersion, const QString &bundleVersion);

QString obs_user_config_dir();

/* Portable OBS (Windows): everything lives in its own folder, installers do not know it. */
bool obs_is_portable();
#ifdef _WIN32
QString obs_install_root();
#endif

/* Folders the plugin checker may move files out of: OBS's install folder, the plugin folders OBS
 * loads from and the plugin settings folder. Everything else is left alone. */
QStringList removable_roots();

