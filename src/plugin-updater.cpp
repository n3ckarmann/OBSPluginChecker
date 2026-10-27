#include "plugin-updater.hpp"
#include "pe-file.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>

#ifdef _WIN32
#include <windows.h>
#endif

bool digest_matches(const QByteArray &data, const QString &digest)
{
	if (!digest.startsWith("sha256:", Qt::CaseInsensitive))
		return false;
	QByteArray hex = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
	return QString::fromLatin1(hex).compare(digest.mid(7), Qt::CaseInsensitive) == 0;
}

namespace {

bool inside(const QString &path, const QString &dir)
{
	return QDir::cleanPath(path).startsWith(QDir::cleanPath(dir) + "/", Qt::CaseInsensitive);
}

bool inside_any(const QString &path, const QStringList &roots)
{
	for (const QString &root : roots)
		if (inside(path, root))
			return true;
	return false;
}

#ifdef _WIN32
QString system_tool(const char *exe)
{
	wchar_t buf[MAX_PATH];
	UINT n = GetSystemDirectoryW(buf, MAX_PATH);
	return n && n < MAX_PATH ? QString::fromWCharArray(buf, int(n)) + "/" + exe : QString();
}

/* tar.exe ships with Windows 10 1803 and later and reads ZIP files. */
bool run_tar(const QStringList &args, QByteArray *out)
{
	QString tar = system_tool("tar.exe");
	if (tar.isEmpty() || !QFileInfo::exists(tar))
		return false;
	QProcess p;
	p.start(tar, args);
	if (!p.waitForFinished(120000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
		return false;
	if (out)
		*out = p.readAllStandardOutput();
	return true;
}

/* Rejects absolute paths, drive letters and ".." anywhere in an entry name. */
bool safe_entry(const QString &name)
{
	if (name.isEmpty() || name.startsWith('/') || name.startsWith('\\') || name.contains(':'))
		return false;
	for (const QString &part : name.split(QRegularExpression("[/\\\\]"), Qt::SkipEmptyParts))
		if (part == "..")
			return false;
	return true;
}

QString installed_binary(const PluginEntry &e)
{
	QFileInfo loc(e.location);
	if (!loc.isDir())
		return loc.exists() ? loc.absoluteFilePath() : QString();
	for (const QString &c : {e.location + "/" + e.name + ".dll", e.location + "/bin/64bit/" + e.name + ".dll"})
		if (QFileInfo::exists(c))
			return QFileInfo(c).absoluteFilePath();
	return {};
}
#endif

QString fail(const QString &jobDir, const char *key)
{
	QDir(jobDir).removeRecursively();
	return key;
}

} // namespace

QString prepare_update(const PluginEntry &e, const QByteArray &zip, const QString &jobRoot, UpdatePlan &plan)
{
#ifndef _WIN32
	(void)e;
	(void)zip;
	(void)jobRoot;
	(void)plan;
	return "Update.WindowsOnly";
#else
	plan = UpdatePlan();
	plan.plugin = e.name;
	QString safeName = QString(e.name).replace(QRegularExpression("[^A-Za-z0-9._-]"), "_");
	plan.jobDir = QDir::cleanPath(jobRoot) + "/" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") +
		      "-" + safeName;
	if (QFileInfo::exists(plan.jobDir))
		return "Update.Failed";
	const QString jobDir = plan.jobDir;
	const QString staging = jobDir + "/new";
	if (!QDir().mkpath(staging))
		return "Update.Failed";

	QString zipPath = jobDir + "/download.zip";
	QFile f(zipPath);
	if (!f.open(QIODevice::WriteOnly) || f.write(zip) != zip.size())
		return fail(jobDir, "Update.Failed");
	f.close();

	/* look at every entry before anything is extracted */
	QByteArray listing;
	if (!run_tar({"-tf", zipPath}, &listing))
		return fail(jobDir, "Update.BadZip");
	for (const QByteArray &line : listing.split('\n'))
		if (!line.trimmed().isEmpty() && !safe_entry(QString::fromUtf8(line.trimmed())))
			return fail(jobDir, "Update.UnsafeZip");
	if (!run_tar({"-xf", zipPath, "-C", staging}, nullptr))
		return fail(jobDir, "Update.BadZip");
	QFile::remove(zipPath);

	/* everything extracted must be a plain file or folder inside the staging folder */
	QStringList extracted;
	QDirIterator all(staging, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
			 QDirIterator::Subdirectories);
	while (all.hasNext()) {
		QFileInfo fi(all.next());
		if (fi.isSymLink() || fi.isJunction() || !inside(fi.absoluteFilePath(), staging))
			return fail(jobDir, "Update.UnsafeZip");
		if (fi.isFile())
			extracted.append(fi.absoluteFilePath());
	}

	/* the plugin's own module, built for 64-bit OBS */
	QStringList modules;
	for (const QString &path : extracted) {
		QFileInfo fi(path);
		if (fi.fileName().compare(e.name + ".dll", Qt::CaseInsensitive) != 0)
			continue;
		if (path.contains("/32bit/", Qt::CaseInsensitive) || path.contains("/x86/", Qt::CaseInsensitive))
			continue;
		if (read_pe(path).exports.contains("obs_module_load"))
			modules.append(path);
	}
	if (modules.size() != 1)
		return fail(jobDir, modules.isEmpty() ? "Update.NoModule" : "Update.Ambiguous");

	const QString srcBin = QFileInfo(modules.first()).absolutePath();
	QString srcData;
	if (srcBin.endsWith("/obs-plugins/64bit", Qt::CaseInsensitive))
		srcData = QDir::cleanPath(srcBin + "/../../data/obs-plugins/" + e.name); // flat layout
	else if (srcBin.endsWith("/bin/64bit", Qt::CaseInsensitive))
		srcData = QDir::cleanPath(srcBin + "/../../data"); // old ProgramData layout
	else
		srcData = srcBin + "/data"; // OBS 33 layout
	if (!QFileInfo(srcData).isDir() || !inside(srcData, staging))
		srcData.clear();

	/* where the installed plugin lives */
	const QString installed = installed_binary(e);
	if (installed.isEmpty() || e.dataPath.isEmpty())
		return fail(jobDir, "Update.NotInstalled");
	const QString tgtBin = QFileInfo(installed).absolutePath();
	const QString tgtData = QDir::cleanPath(QFileInfo(e.dataPath).absoluteFilePath());
	const QStringList roots = removable_roots();

	auto add = [&](const QString &source, const QString &target) -> bool {
		if (!inside_any(target, roots))
			return false;
		plan.files.append({source, target, QFileInfo::exists(target)});
		return true;
	};

	for (const QFileInfo &fi : QDir(srcBin).entryInfoList(QDir::Files | QDir::Hidden)) {
		QString target = tgtBin + "/" + fi.fileName();
		/* never overwrite a file that belongs to something else (OBS itself or another plugin) */
		if (QFileInfo::exists(target) && !e.files.contains(target, Qt::CaseInsensitive))
			return fail(jobDir, "Update.Conflict");
		if (!add(fi.absoluteFilePath(), target))
			return fail(jobDir, "Update.OutsideObs");
	}
	if (!srcData.isEmpty()) {
		QDirIterator it(srcData, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
		while (it.hasNext()) {
			QString source = it.next();
			if (!add(source, tgtData + "/" + QDir(srcData).relativeFilePath(source)))
				return fail(jobDir, "Update.OutsideObs");
		}
	}

	QSet<QString> used;
	for (const UpdateFile &u : plan.files)
		used.insert(u.source);
	for (const QString &path : extracted)
		if (!used.contains(path))
			plan.ignored.append(QDir(staging).relativeFilePath(path));
	return {};
#endif
}
