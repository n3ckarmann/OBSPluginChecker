#pragma once

#include <QString>
#include <QStringList>

/* Minimal reader for Windows PE files (DLLs). Works on the file contents only, nothing is loaded. */
struct PeInfo {
	bool valid = false;
	QStringList exports; // exported function names
	QStringList imports; // names of imported DLLs, as written in the file
};

PeInfo read_pe(const QString &path);
