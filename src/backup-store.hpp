#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

/* Job folders that updates and removals leave in the plugin's config folder: the old files plus the
 * scripts to restore them. */
struct BackupJob {
	QString dir;
	QString kind;   // "update" or "remove"
	QString plugin; // module name from the folder name
	QDateTime when;
	qint64 bytes = 0;
	bool pending = false; // the job has not run yet (waits for OBS to exit), must not be deleted
};

/* updatesRoot and removedRoot are <config>/updates and <config>/removed. Newest first. */
QList<BackupJob> list_backups(const QString &updatesRoot, const QString &removedRoot);

/* Deletes a job folder for good. Refuses pending jobs and anything that is not a job folder directly
 * inside one of the two roots. */
bool delete_backup(const BackupJob &job, const QString &updatesRoot, const QString &removedRoot);
