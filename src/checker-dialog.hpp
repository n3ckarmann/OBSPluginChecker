#pragma once

#include <QDialog>

class QLabel;
class QPushButton;
class QTableWidget;
class UpdateChecker;
struct PluginEntry;

class CheckerDialog : public QDialog {
public:
	static constexpr int kColumns = 6;

	CheckerDialog(QWidget *parent, UpdateChecker *checker);

	void refresh();

private:
	const PluginEntry *selected() const;
	void updateButtons();
	void mapRepo();
	void showFiles();
	bool removePlugin(const PluginEntry &e);
	void updatePlugin(const PluginEntry &e);
	void confirmUpdate(const PluginEntry &e, const QByteArray &zip);
	void updateDetails();
	QStringList hintsFor(const PluginEntry &e) const;

	UpdateChecker *checker_;
	QLabel *header_;
	QLabel *footer_;
	QLabel *details_;
	QTableWidget *table_;
	QPushButton *download_;
	QPushButton *release_;
	QPushButton *map_;
	QPushButton *reveal_;
	QPushButton *files_;
	QPushButton *update_;
	bool downloading_ = false;
	QPushButton *recheck_;
};
