/* Headless test: starts libobs from OBS.app, opens the built plugin with obs_open_module (the same
 * version/export checks OBS does), registers the installed third-party plugins without initializing them,
 * then runs the scanner and GitHub check and prints the result. */
#include "../src/checker-dialog.hpp"
#include "../src/update-checker.hpp"

#include <obs-module.h>

extern "C" void obs_module_set_pointer(obs_module_t *module);

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QTimer>

#include <cstdio>

static int open_module(const QString &bundle, obs_module_t **mod)
{
	QString name = QFileInfo(bundle).completeBaseName();
	QByteArray bin = (bundle + "/Contents/MacOS/" + name).toUtf8();
	QByteArray data = (bundle + "/Contents/Resources").toUtf8();
	return obs_open_module(mod, bin.constData(), data.constData());
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: harness <obs-plugin-checker.plugin>\n");
		return 2;
	}
	QApplication app(argc, argv);
	QByteArray configDir = qgetenv("HARNESS_CONFIG");
	if (!obs_startup("en-US", configDir.isEmpty() ? nullptr : configDir.constData(), nullptr)) {
		fprintf(stderr, "obs_startup failed\n");
		return 1;
	}
	printf("libobs %s\n", obs_get_version_string());

	obs_module_t *self = nullptr;
	int rc = open_module(QString::fromUtf8(argv[1]), &self);
	printf("obs_open_module(obs-plugin-checker) = %d (%s)\n", rc, rc == MODULE_SUCCESS ? "OK" : "FEHLER");
	if (rc != MODULE_SUCCESS)
		return 1;
	obs_module_set_pointer(self);
	obs_module_set_locale("de-DE");

	QDir plugins(QDir::homePath() + "/Library/Application Support/obs-studio/plugins");
	for (const QFileInfo &fi : plugins.entryInfoList({"*.plugin"}, QDir::Dirs)) {
		if (fi.fileName() == "obs-plugin-checker.plugin")
			continue;
		obs_module_t *m = nullptr;
		printf("obs_open_module(%s) = %d\n", qPrintable(fi.fileName()), open_module(fi.absoluteFilePath(), &m));
	}

	auto checkerPtr = new UpdateChecker();
	UpdateChecker &checker = *checkerPtr;
	checker.changed = [&]() {
		if (!checker.running())
			QTimer::singleShot(0, &app, &QApplication::quit);
	};
	checker.start(argc > 2 && argv[2][0] == 'f');
	if (checker.running())
		app.exec();

	printf("\nOBS installiert %s, neueste %s%s\n", qPrintable(checker.obsInstalled), qPrintable(checker.obsLatest),
	       checker.rateLimited ? " (GitHub-Limit!)" : "");
	for (const PluginEntry &e : checker.plugins) {
		printf("\n%s  [status %d]\n", qPrintable(e.name), int(e.status));
		printf("  Ort: %s\n  Version: %s  neueste: %s  repo: %s\n", qPrintable(e.location), qPrintable(e.version),
		       qPrintable(e.latest), qPrintable(e.repo));
		printf("  geladen: %d  deaktiviert: %d  legacy: %d  archiviert: %d  vorOBSMajor: %d\n", e.loaded,
		       e.disabled, e.legacy, e.archived, e.releaseBeforeObsMajor);
		for (const QString &d : e.duplicates)
			printf("  Kopie: %s\n", qPrintable(d));
		if (!e.downloadUrl.isEmpty())
			printf("  Download: %s\n", qPrintable(e.downloadUrl));
		if (!e.note.isEmpty())
			printf("  Notiz: %s\n", obs_module_text(e.note.toUtf8().constData()));
	}

	if (argc > 3) {
		auto dialog = new CheckerDialog(nullptr, &checker);
		dialog->show();
		app.processEvents();
		dialog->grab().save(QString::fromUtf8(argv[3]));
		printf("\nScreenshot: %s\n", argv[3]);
		delete dialog;
	}
	delete checkerPtr; /* before obs_shutdown, like on OBS_FRONTEND_EVENT_EXIT */
	obs_shutdown();
	return 0;
}
