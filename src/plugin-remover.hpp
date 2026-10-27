#pragma once

#include <QList>
#include <QString>
#include <QStringList>

/* Moves a plugin's files once OBS has exited. OBS keeps plugin libraries open (and Windows locks them),
 * so a small script waits for this OBS process to end and then moves the files. Every job folder also
 * gets a script that undoes it. Nothing is deleted. */
struct RemovalPlan {
	QString plugin;
	QStringList files;   // files to move
	QStringList cleanup; // folders to remove afterwards if they are empty
	QString backupRoot;  // e.g. <config>/plugin_config/obs-plugin-checker/removed
};

/* Replaces a plugin's files with the ones from a release ZIP (see plugin-updater). */
struct UpdateFile {
	QString source; // extracted file in the job folder
	QString target; // where it goes in OBS's folder
	bool replaces = false;
};

struct UpdatePlan {
	QString plugin;
	QString jobDir; // job folder with the extracted ZIP; scripts and backup go here too
	QList<UpdateFile> files;
	QStringList ignored; // ZIP entries that do not belong to the plugin, relative paths
};

struct RemovalResult {
	bool ok = false;
	QString backupDir; // job folder with scripts, log and the moved files
	bool elevated = false; // a file is in a folder that needs admin rights (Windows asks via UAC)
};

RemovalResult schedule_removal(const RemovalPlan &plan);
RemovalResult schedule_update(const UpdatePlan &plan);
