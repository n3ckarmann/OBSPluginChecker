#include "backup-store.hpp"

#include <algorithm>

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>

namespace {

/* job_dir() in plugin-remover: yyyyMMdd-HHmmss-<plugin> */
const QRegularExpression &job_name()
{
	static const QRegularExpression re("^(\\d{8}-\\d{6})-([A-Za-z0-9._-]+)$");
	return re;
}

qint64 folder_size(const QString &dir)
{
	qint64 total = 0;
	QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
	while (it.hasNext()) {
		it.next();
		total += it.fileInfo().size();
	}
	return total;
}

void collect(QList<BackupJob> &out, const QString &root, const QString &kind)
{
	if (root.isEmpty())
		return;
	for (const QFileInfo &fi : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		if (fi.isSymLink())
			continue;
		auto m = job_name().match(fi.fileName());
		if (!m.hasMatch())
			continue;
		BackupJob job;
		job.dir = fi.absoluteFilePath();
		job.kind = kind;
		job.plugin = m.captured(2);
		job.when = QDateTime::fromString(m.captured(1), "yyyyMMdd-HHmmss");
		job.bytes = folder_size(job.dir);
		/* the job script writes its log only after OBS has exited and the files were moved */
		job.pending = !QFileInfo::exists(job.dir + "/" + kind + ".log");
		out.append(job);
	}
}

bool directly_inside(const QString &dir, const QString &root)
{
	if (root.isEmpty())
		return false;
	QString parent = QDir::cleanPath(QFileInfo(dir).absolutePath());
	return parent.compare(QDir::cleanPath(QFileInfo(root).absoluteFilePath()), Qt::CaseInsensitive) == 0;
}

} // namespace

QList<BackupJob> list_backups(const QString &updatesRoot, const QString &removedRoot)
{
	QList<BackupJob> jobs;
	collect(jobs, updatesRoot, "update");
	collect(jobs, removedRoot, "remove");
	std::sort(jobs.begin(), jobs.end(), [](const BackupJob &a, const BackupJob &b) { return a.when > b.when; });
	return jobs;
}

bool delete_backup(const BackupJob &job, const QString &updatesRoot, const QString &removedRoot)
{
	if (job.pending)
		return false;
	QFileInfo fi(job.dir);
	if (!fi.isDir() || fi.isSymLink() || !job_name().match(fi.fileName()).hasMatch())
		return false;
	const QString root = job.kind == "update" ? updatesRoot : removedRoot;
	if (!directly_inside(job.dir, root))
		return false;
	/* checked again on disk: the job may have been started since the list was made */
	if (!QFileInfo::exists(job.dir + "/" + job.kind + ".log"))
		return false;
	return QDir(job.dir).removeRecursively();
}
