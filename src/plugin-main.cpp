#include "checker-dialog.hpp"
#include "update-checker.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAction>
#include <QMainWindow>
#include <QPointer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-plugin-checker", "en-US")
OBS_MODULE_AUTHOR("n3ckarmann")

static UpdateChecker *checker = nullptr;
static QPointer<QAction> menuAction;
static QPointer<CheckerDialog> dialog;

const char *obs_module_name(void)
{
	return "Plugin Checker";
}

const char *obs_module_description(void)
{
	return "Checks installed third-party plugins for new versions on GitHub.";
}

static void update_menu_text()
{
	if (!menuAction || !checker)
		return;
	int updates = checker->running() ? 0 : checker->updateCount();
	QString text = QString::fromUtf8(obs_module_text("PluginChecker"));
	if (updates > 0)
		text = QString::fromUtf8(obs_module_text(updates == 1 ? "Menu.Updates.One" : "Menu.Updates"))
			       .arg(text)
			       .arg(updates);
	menuAction->setText(text);
}

static void on_changed()
{
	update_menu_text();
	if (dialog)
		dialog->refresh();
	if (checker && !checker->running() && checker->lastCheck.isValid()) {
		int updates = checker->updateCount();
		blog(LOG_INFO, "[obs-plugin-checker] %d third-party plugin(s), %d update(s) available",
		     int(checker->plugins.size()), updates);
	}
}

static void show_dialog()
{
	if (!checker)
		return;
	if (!dialog) {
		auto main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		dialog = new CheckerDialog(main, checker);
	}
	if (!checker->running() && !checker->lastCheck.isValid())
		checker->start(false);
	dialog->show();
	dialog->raise();
	dialog->activateWindow();
}

static void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		checker = new UpdateChecker();
		checker->changed = on_changed;
		checker->start(false);
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		if (dialog)
			dialog->close();
		delete checker;
		checker = nullptr;
	}
}

bool obs_module_load(void)
{
	auto action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("PluginChecker")));
	menuAction = action;
	QObject::connect(action, &QAction::triggered, show_dialog);
	obs_frontend_add_event_callback(frontend_event, nullptr);
	blog(LOG_INFO, "[obs-plugin-checker] loaded version %s", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	delete checker;
	checker = nullptr;
}
