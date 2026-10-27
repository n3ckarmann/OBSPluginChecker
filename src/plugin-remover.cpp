#include "plugin-remover.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryFile>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

/* C:/Program Files/obs-studio/x.dll -> <backup>/files/C/Program Files/obs-studio/x.dll */
QString backup_path(const QString &backupDir, const QString &file)
{
	QString clean = QDir::cleanPath(QFileInfo(file).absoluteFilePath());
	clean.remove(':');
	while (clean.startsWith('/'))
		clean.remove(0, 1);
	return backupDir + "/files/" + clean;
}

/* The folder itself, or for a folder that does not exist yet the closest parent that does. */
bool folder_writable(const QString &dir)
{
	QFileInfo fi(QDir::cleanPath(dir));
	while (!fi.exists() && fi.absolutePath() != fi.absoluteFilePath())
		fi = QFileInfo(fi.absolutePath());
	QTemporaryFile probe(fi.absoluteFilePath() + "/.obs-plugin-checker-XXXXXX");
	return probe.open();
}

bool write_text(const QString &path, const QString &text)
{
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly))
		return false;
	QByteArray data = text.toUtf8();
#ifdef _WIN32
	f.write("\xEF\xBB\xBF"); // UTF-8 BOM, so Windows PowerShell 5 reads umlauts in paths correctly
	data.replace("\n", "\r\n");
#endif
	f.write(data);
	return f.commit();
}

using PathPairs = QList<QPair<QString, QString>>;

struct FileOps {
	PathPairs moves;
	QStringList cleanup; // folders to remove if they end up empty
};

#ifdef _WIN32
QString ps_quote(const QString &s)
{
	return "'" + QString(s).replace("'", "''") + "'";
}

QString ps_pairs(const char *var, const PathPairs &pairs)
{
	QString s = QString("$%1 = @(\n").arg(var);
	for (const auto &p : pairs)
		s += QString("  ,@(%1, %2)\n").arg(ps_quote(QDir::toNativeSeparators(p.first)),
						  ps_quote(QDir::toNativeSeparators(p.second)));
	return s + ")\n";
}

QString ps_list(const char *var, const QStringList &paths)
{
	QString s = QString("$%1 = @(\n").arg(var);
	for (const QString &p : paths)
		s += "  " + ps_quote(QDir::toNativeSeparators(p)) + "\n";
	return s + ")\n";
}

QString ps_script(const FileOps &ops, const QString &logFile, qint64 waitPid)
{
	QString s;
	s += "$ErrorActionPreference = 'Continue'\n";
	if (waitPid > 0)
		s += QString("Wait-Process -Id %1 -ErrorAction SilentlyContinue\nStart-Sleep -Seconds 2\n").arg(waitPid);
	s += "$log = @()\n";
	s += ps_pairs("moves", ops.moves) + ps_list("cleanup", ops.cleanup);
	s += "foreach ($m in $moves) {\n"
	     "  if (-not (Test-Path -LiteralPath $m[0])) { continue }\n"
	     "  if (Test-Path -LiteralPath $m[1]) { $log += 'BELEGT  ' + $m[1]; continue }\n"
	     "  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $m[1]) | Out-Null\n"
	     "  try {\n"
	     "    Move-Item -LiteralPath $m[0] -Destination $m[1] -ErrorAction Stop\n"
	     "    $log += 'OK      ' + $m[0]\n"
	     "  }\n"
	     "  catch { $log += 'FEHLER  ' + $m[0] + '  ' + $_.Exception.Message }\n"
	     "}\n"
	     "foreach ($d in $cleanup) {\n"
	     "  if (-not (Test-Path -LiteralPath $d)) { continue }\n"
	     "  Get-ChildItem -LiteralPath $d -Recurse -Directory -Force | Sort-Object { $_.FullName.Length } -Descending |\n"
	     "    Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Force) } | Remove-Item -Force\n"
	     "  if (-not (Get-ChildItem -LiteralPath $d -Force)) { Remove-Item -LiteralPath $d -Force }\n"
	     "}\n";
	s += "$log | Set-Content -LiteralPath " + ps_quote(QDir::toNativeSeparators(logFile)) + " -Encoding UTF8\n";
	return s;
}

bool launch_powershell(const QString &script, bool elevated)
{
	QString args = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
		       QDir::toNativeSeparators(script) + "\"";
	std::wstring wargs = args.toStdWString();
	auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, elevated ? L"runas" : L"open", L"powershell.exe",
							  wargs.c_str(), nullptr, SW_HIDE));
	return r > 32;
}
#else
QString sh_quote(const QString &s)
{
	return "'" + QString(s).replace("'", "'\\''") + "'";
}

QString sh_script(const FileOps &ops, const QString &logFile, qint64 waitPid)
{
	QString s = "#!/bin/sh\n";
	if (waitPid > 0)
		s += QString("while kill -0 %1 2>/dev/null; do sleep 1; done\n").arg(waitPid);
	s += "LOG=" + sh_quote(logFile) + "\n: > \"$LOG\"\n";
	s += "move() {\n"
	     "  [ -e \"$1\" ] || return 0\n"
	     "  if [ -e \"$2\" ]; then echo \"BELEGT  $2\" >> \"$LOG\"; return 0; fi\n"
	     "  mkdir -p \"$(dirname \"$2\")\" && mv \"$1\" \"$2\" && echo \"OK      $1\" >> \"$LOG\" || echo \"FEHLER  $1\" >> \"$LOG\"\n"
	     "}\n";
	for (const auto &m : ops.moves)
		s += "move " + sh_quote(m.first) + " " + sh_quote(m.second) + "\n";
	for (const QString &dir : ops.cleanup)
		s += "[ -d " + sh_quote(dir) + " ] && find " + sh_quote(dir) + " -depth -type d -empty -delete\n";
	return s;
}
#endif

/* Writes <job>/<name>.ps1|sh and <job>/<undoName>.ps1|sh and starts the first one, which waits for OBS. */
bool schedule(RemovalResult &result, const FileOps &run, const FileOps &undo, const QString &name,
	      const QString &undoName)
{
	for (const auto &m : run.moves)
		result.elevated = result.elevated || !folder_writable(QFileInfo(m.first).absolutePath()) ||
				  !folder_writable(QFileInfo(m.second).absolutePath());

	const QString dir = result.backupDir;
	qint64 pid = QCoreApplication::applicationPid();
#ifdef _WIN32
	QString runScript = dir + "/" + name + ".ps1";
	if (!write_text(runScript, ps_script(run, dir + "/" + name + ".log", pid)) ||
	    !write_text(dir + "/" + undoName + ".ps1", ps_script(undo, dir + "/" + undoName + ".log", 0)))
		return false;
	return launch_powershell(runScript, result.elevated);
#else
	QString runScript = dir + "/" + name + ".sh";
	QString undoScript = dir + "/" + undoName + ".sh";
	if (!write_text(runScript, sh_script(run, dir + "/" + name + ".log", pid)) ||
	    !write_text(undoScript, sh_script(undo, dir + "/" + undoName + ".log", 0)))
		return false;
	QFile::setPermissions(undoScript, QFile::permissions(undoScript) | QFileDevice::ExeOwner);
	return QProcess::startDetached("/bin/sh", {runScript});
#endif
}

QString job_dir(const QString &root, const QString &plugin)
{
	QString safeName = QString(plugin).replace(QRegularExpression("[^A-Za-z0-9._-]"), "_");
	return root + "/" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + "-" + safeName;
}

} // namespace

RemovalResult schedule_removal(const RemovalPlan &plan)
{
	RemovalResult result;
	result.backupDir = job_dir(plan.backupRoot, plan.plugin);
	if (!QDir().mkpath(result.backupDir))
		return result;

	FileOps run, undo;
	for (const QString &file : plan.files) {
		QString target = backup_path(result.backupDir, file);
		run.moves.append({file, target});
		undo.moves.append({target, file});
	}
	run.cleanup = plan.cleanup;
	undo.cleanup << result.backupDir + "/files";

	QString list;
	for (const QString &file : plan.files)
		list += QDir::toNativeSeparators(file) + "\n";
	write_text(result.backupDir + "/files.txt", list);

	result.ok = schedule(result, run, undo, "remove", "restore");
	return result;
}

RemovalResult schedule_update(const UpdatePlan &plan)
{
	RemovalResult result;
	result.backupDir = plan.jobDir;
	if (!QFileInfo(plan.jobDir).isDir())
		return result;

	FileOps run, undo;
	/* old files into the backup first, then the new ones in */
	for (const UpdateFile &f : plan.files)
		if (f.replaces)
			run.moves.append({f.target, backup_path(plan.jobDir, f.target)});
	for (const UpdateFile &f : plan.files)
		run.moves.append({f.source, f.target});
	run.cleanup << plan.jobDir + "/new";

	for (const UpdateFile &f : plan.files)
		undo.moves.append({f.target, backup_path(plan.jobDir + "/undone", f.target)});
	for (const UpdateFile &f : plan.files)
		if (f.replaces)
			undo.moves.append({backup_path(plan.jobDir, f.target), f.target});
	undo.cleanup << plan.jobDir + "/files";

	QString list;
	for (const UpdateFile &f : plan.files)
		list += (f.replaces ? "ERSETZT " : "NEU     ") + QDir::toNativeSeparators(f.target) + "\n";
	for (const QString &path : plan.ignored)
		list += "IGNORIERT " + QDir::toNativeSeparators(path) + "\n";
	write_text(plan.jobDir + "/files.txt", list);

	result.ok = schedule(result, run, undo, "update", "restore");
	return result;
}
