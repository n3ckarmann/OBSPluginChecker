#include "checker-dialog.hpp"
#include "backup-store.hpp"
#include "update-checker.hpp"
#include "plugin-remover.hpp"
#include "plugin-updater.hpp"

#include <obs-module.h>

#include <algorithm>

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#ifdef _WIN32
static const char *kRestoreScript = "restore.ps1";
#else
static const char *kRestoreScript = "restore.sh";
#endif

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* "<key>.One" for exactly one, "<key>" (with %1) otherwise: "1 Update", "3 Updates". */
static QString count_text(const char *key, qsizetype n)
{
	const char *out = nullptr;
	if (n == 1 && obs_module_get_string((QByteArray(key) + ".One").constData(), &out))
		return QString::fromUtf8(out);
	return QString::fromUtf8(obs_module_text(key)).arg(n);
}

/* Notes and errors are stored as locale keys; anything without a translation is shown as-is. */
static QString text_or_key(const QString &key)
{
	if (key.isEmpty())
		return {};
	const char *out = nullptr;
	/* A platform-specific variant ("Elgato.Note.macOS") wins over the generic text. */
#if defined(_WIN32)
	const char *platform = nullptr;
#elif defined(__APPLE__)
	const char *platform = ".macOS";
#else
	const char *platform = ".Linux";
#endif
	if (platform && obs_module_get_string((key + platform).toUtf8().constData(), &out))
		return QString::fromUtf8(out);
	if (obs_module_get_string(key.toUtf8().constData(), &out))
		return QString::fromUtf8(out);
	return key;
}

/* Links come from GitHub, from repo mappings and from other plugins' manifest.json, so only web links
 * are opened; anything else (file:, custom protocol handlers) is ignored. */
static void open_web_link(const QString &link)
{
	QUrl url(link);
	if (url.isValid() && (url.scheme() == "https" || url.scheme() == "http") && !url.host().isEmpty())
		QDesktopServices::openUrl(url);
}

/* Release downloads only from GitHub itself. */
static void open_download(const QString &link)
{
	QUrl url(link);
	if (url.scheme() == "https" && url.host() == "github.com")
		QDesktopServices::openUrl(url);
}

/* Paths for display: the user folder becomes ~, so the dialog can be shown on stream without the
 * Windows account name. Copy and open still use the real path. */
static QString display_path(const QString &path)
{
	QString native = QDir::toNativeSeparators(path);
	QString home = QDir::toNativeSeparators(QDir::homePath());
	if (!home.isEmpty() && native.startsWith(home, Qt::CaseInsensitive))
		return "~" + native.mid(home.size());
	return native;
}

static QString status_text(PluginEntry::Status s)
{
	switch (s) {
	case PluginEntry::Status::Checking:
		return T("Status.Checking");
	case PluginEntry::Status::UpToDate:
		return T("Status.UpToDate");
	case PluginEntry::Status::Update:
		return T("Status.Update");
	case PluginEntry::Status::Check:
		return T("Status.Check");
	case PluginEntry::Status::Manual:
		return T("Status.Manual");
	case PluginEntry::Status::Error:
		return T("Status.Error");
	default:
		return T("Status.Unknown");
	}
}

static QColor status_color(PluginEntry::Status s)
{
	switch (s) {
	case PluginEntry::Status::UpToDate:
		return QColor(0x3c, 0xb3, 0x71);
	case PluginEntry::Status::Update:
		return QColor(0xf0, 0xa0, 0x20);
	case PluginEntry::Status::Error:
		return QColor(0xe0, 0x50, 0x50);
	default:
		return QColor(0x80, 0x9a, 0xb8);
	}
}

enum class Future { Ok, OldPlace, Broken, Leftover };

static Future future_of(const PluginEntry &e)
{
	if (e.leftover)
		return Future::Leftover;
	if (!e.missingLibraries.isEmpty() || (!e.loaded && !e.disabled))
		return Future::Broken;
	if (e.legacy)
		return Future::OldPlace;
	return Future::Ok;
}

static QString future_text(Future f)
{
	switch (f) {
	case Future::Ok:
		return T("Future.Ok");
	case Future::OldPlace:
		return T("Future.OldPlace");
	case Future::Broken:
		return T("Future.Broken");
	default:
		return T("Future.Leftover");
	}
}

static QColor future_color(Future f)
{
	switch (f) {
	case Future::Ok:
		return QColor(0x3c, 0xb3, 0x71);
	case Future::OldPlace:
		return QColor(0xf0, 0xa0, 0x20);
	case Future::Broken:
		return QColor(0xe0, 0x50, 0x50);
	default:
		return QColor(0x80, 0x9a, 0xb8);
	}
}

static QString usage_text(const PluginEntry &e)
{
	if (e.leftover)
		return "–";
	if (!e.usage.isEmpty()) {
		int n = 0;
		for (int count : e.usage)
			n += count;
		return T("Used.Yes").arg(n);
	}
	return e.types.isEmpty() ? T("Used.Unknown") : T("Used.No");
}

CheckerDialog::CheckerDialog(QWidget *parent, UpdateChecker *checker) : QDialog(parent), checker_(checker)
{
	setWindowTitle(T("PluginChecker"));
	setAttribute(Qt::WA_DeleteOnClose);
	resize(980, 640);

	header_ = new QLabel(this);
	header_->setTextFormat(Qt::RichText);
	connect(header_, &QLabel::linkActivated, this, [](const QString &link) { open_web_link(link); });

	table_ = new QTableWidget(0, kColumns, this);
	table_->setHorizontalHeaderLabels({T("Column.Plugin"), T("Column.Installed"), T("Column.Latest"),
					   T("Column.Status"), T("Column.Used"), T("Column.Future")});
	table_->setSelectionBehavior(QAbstractItemView::SelectRows);
	table_->setSelectionMode(QAbstractItemView::SingleSelection);
	table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table_->setAlternatingRowColors(true);
	table_->setWordWrap(false);
	table_->setSortingEnabled(true);
	table_->verticalHeader()->hide();
	/* All columns can be dragged; widths start from the content (fitColumns) or the saved layout. Together
	 * they always fill the table's width: dragging one takes the room from the columns to its right. */
	table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
	table_->horizontalHeader()->setStretchLastSection(false);
	table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	connect(table_->horizontalHeader(), &QHeaderView::sectionResized, this,
		[this](int column, int oldSize, int newSize) { columnResized(column, oldSize, newSize); });
	table_->viewport()->installEventFilter(this);

	details_ = new QLabel(this);
	details_->setTextFormat(Qt::RichText);
	details_->setWordWrap(true);
	details_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	details_->setMargin(8);
	/* Long hint lists (Stream Deck) are taller than the panel: scroll instead of clipping the title. */
	auto detailsScroll = new QScrollArea(this);
	detailsScroll->setWidget(details_);
	detailsScroll->setWidgetResizable(true);
	detailsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	detailsScroll->setFrameShape(QFrame::StyledPanel);
	detailsScroll->setMinimumHeight(60);

	/* Divider between list and details, so the list can take as much room as wanted. */
	splitter_ = new QSplitter(Qt::Vertical, this);
	splitter_->addWidget(table_);
	splitter_->addWidget(detailsScroll);
	splitter_->setStretchFactor(0, 1);
	splitter_->setStretchFactor(1, 0);
	splitter_->setCollapsible(0, false);
	splitter_->setCollapsible(1, false);
	splitter_->setSizes({440, 130});

	download_ = new QPushButton(T("Button.Download"), this);
	release_ = new QPushButton(T("Button.Release"), this);
	map_ = new QPushButton(T("Button.MapRepo"), this);
	reveal_ = new QPushButton(T("Button.Reveal"), this);
	files_ = new QPushButton(T("Button.Files"), this);
	update_ = new QPushButton(T("Button.Update"), this);
	recheck_ = new QPushButton(T("Button.Recheck"), this);

	footer_ = new QLabel(this);
	footer_->setWordWrap(true);
	footer_->setStyleSheet("color: gray;");

	auto buttons = new QHBoxLayout();
	buttons->addWidget(update_);
	buttons->addWidget(download_);
	buttons->addWidget(release_);
	buttons->addWidget(map_);
	buttons->addWidget(reveal_);
	buttons->addWidget(files_);
	buttons->addStretch();
	buttons->addWidget(recheck_);
	auto close = new QPushButton(T("Button.Close"), this);
	buttons->addWidget(close);

	auto layout = new QVBoxLayout(this);
	layout->addWidget(header_);
	layout->addWidget(splitter_, 1);
	layout->addLayout(buttons);
	layout->addWidget(footer_);

	connect(close, &QPushButton::clicked, this, &QDialog::reject);
	connect(table_, &QTableWidget::itemSelectionChanged, this, [this]() {
		updateButtons();
		updateDetails();
	});
	connect(table_, &QTableWidget::itemDoubleClicked, this, [this]() { release_->click(); });
	connect(download_, &QPushButton::clicked, this, [this]() {
		if (auto e = selected(); e && !e->downloadUrl.isEmpty())
			open_download(e->downloadUrl);
	});
	connect(release_, &QPushButton::clicked, this, [this]() {
		auto e = selected();
		if (!e)
			return;
		QString url = !e->releaseUrl.isEmpty() ? e->releaseUrl
			      : !e->repo.isEmpty()     ? "https://github.com/" + e->repo
						       : e->homepage;
		if (!url.isEmpty())
			open_web_link(url);
	});
	connect(reveal_, &QPushButton::clicked, this, [this]() {
		if (auto e = selected(); e && !e->location.isEmpty())
			QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(e->location).absolutePath()));
	});
	connect(map_, &QPushButton::clicked, this, [this]() { mapRepo(); });
	connect(files_, &QPushButton::clicked, this, [this]() { showFiles(); });
	connect(update_, &QPushButton::clicked, this, [this]() {
		if (auto e = selected())
			updatePlugin(*e);
	});
	connect(recheck_, &QPushButton::clicked, this, [this]() { checker_->start(true); });

	restoreLayout();
	refresh();
	/* the viewport has its real width only once the dialog is shown */
	QTimer::singleShot(0, this, [this]() {
		fitColumns();
		fillWidth(0);
	});
}

/* Window size, column widths and the divider are kept in the plugin's own config folder. */
static QString layout_file()
{
	char *path = obs_module_config_path("dialog.ini");
	QString file = QString::fromUtf8(path);
	bfree(path);
	return file;
}

void CheckerDialog::restoreLayout()
{
	QSettings settings(layout_file(), QSettings::IniFormat);
	if (QByteArray geometry = settings.value("geometry").toByteArray(); !geometry.isEmpty())
		restoreGeometry(geometry);
	if (QByteArray split = settings.value("splitter").toByteArray(); !split.isEmpty())
		splitter_->restoreState(split);
	QByteArray columns = settings.value("columns").toByteArray();
	if (!columns.isEmpty() && table_->horizontalHeader()->restoreState(columns))
		columnsFitted_ = true;
}

void CheckerDialog::saveLayout() const
{
	QDir().mkpath(QFileInfo(layout_file()).absolutePath());
	QSettings settings(layout_file(), QSettings::IniFormat);
	settings.setValue("geometry", saveGeometry());
	settings.setValue("splitter", splitter_->saveState());
	if (columnsFitted_)
		settings.setValue("columns", table_->horizontalHeader()->saveState());
}

void CheckerDialog::done(int result)
{
	saveLayout();
	QDialog::done(result);
}

/* First start without a saved layout: columns fit their content, the plugin name gets the rest. */
void CheckerDialog::fitColumns()
{
	if (columnsFitted_ || !isVisible() || table_->rowCount() == 0 || checker_->running())
		return;
	table_->resizeColumnsToContents();
	int others = 0;
	for (int c = 1; c < kColumns; c++)
		others += table_->columnWidth(c);
	int free = table_->viewport()->width() - others;
	if (free > table_->columnWidth(0))
		table_->setColumnWidth(0, free);
	columnsFitted_ = true;
	fillWidth(0);
}

/* A dragged column takes its extra room from the columns to its right, starting with the last one, or
 * gives freed room to the last column. The last column always ends at the right edge. */
void CheckerDialog::columnResized(int column, int oldSize, int newSize)
{
	(void)oldSize;
	int width = table_->viewport()->width();
	if (fitting_ || width <= 0)
		return;
	auto header = table_->horizontalHeader();
	const int last = kColumns - 1;
	const int minSize = header->minimumSectionSize();
	int delta = header->length() - width;
	if (delta == 0)
		return;
	fitting_ = true;
	if (column == last) {
		header->resizeSection(last, std::max(minSize, newSize - delta));
	} else if (delta < 0) {
		header->resizeSection(last, header->sectionSize(last) - delta);
	} else {
		for (int c = last; c > column && delta > 0; c--) {
			int take = std::min(delta, header->sectionSize(c) - minSize);
			if (take > 0) {
				header->resizeSection(c, header->sectionSize(c) - take);
				delta -= take;
			}
		}
		if (delta > 0)
			header->resizeSection(column, newSize - delta);
	}
	fitting_ = false;
}

/* The table got wider or narrower: all columns grow or shrink in proportion, the last one takes the rest. */
void CheckerDialog::fillWidth(int oldWidth)
{
	int width = table_->viewport()->width();
	if (!columnsFitted_ || width <= 0)
		return;
	auto header = table_->horizontalHeader();
	const int last = kColumns - 1;
	const int minSize = header->minimumSectionSize();
	fitting_ = true;
	int total = oldWidth > 0 ? oldWidth : header->length();
	if (total > 0 && total != width) {
		for (int c = 0; c < last; c++)
			header->resizeSection(c, std::max(minSize, int(qint64(header->sectionSize(c)) * width / total)));
	}
	int others = 0;
	for (int c = 0; c < last; c++)
		others += header->sectionSize(c);
	header->resizeSection(last, std::max(minSize, width - others));
	fitting_ = false;
}

bool CheckerDialog::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == table_->viewport() && event->type() == QEvent::Resize) {
		int width = table_->viewport()->width();
		if (width != viewportWidth_) {
			int old = viewportWidth_;
			viewportWidth_ = width;
			fillWidth(old);
		}
	}
	return QDialog::eventFilter(watched, event);
}

const PluginEntry *CheckerDialog::selected() const
{
	int row = table_->currentRow();
	auto item = row >= 0 ? table_->item(row, 0) : nullptr;
	if (!item || !item->isSelected())
		return nullptr;
	int i = item->data(Qt::UserRole).toInt();
	return i >= 0 && i < checker_->plugins.size() ? &checker_->plugins[i] : nullptr;
}

/* Also offered when the installed version is unknown: the user decides whether the latest release is newer. */
static bool offers_update(const PluginEntry &e)
{
	return !e.downloadUrl.isEmpty() &&
	       (e.status == PluginEntry::Status::Update || e.status == PluginEntry::Status::Check);
}

void CheckerDialog::updateButtons()
{
	auto e = selected();
	download_->setEnabled(e && offers_update(*e));
	release_->setEnabled(e && (!e->releaseUrl.isEmpty() || !e->repo.isEmpty() || !e->homepage.isEmpty()));
	map_->setEnabled(e && !checker_->running());
	reveal_->setEnabled(e && !e->location.isEmpty());
	files_->setEnabled(e && !e->files.isEmpty());
#ifdef _WIN32
	bool canUpdate = e && offers_update(*e) && !e->downloadIsInstaller && !downloading_;
	update_->setEnabled(canUpdate);
	update_->setToolTip(e && e->downloadIsInstaller ? T("Update.InstallerOnly") : T("Update.Tooltip"));
#else
	update_->hide();
#endif
	recheck_->setEnabled(!checker_->running());
}

QStringList CheckerDialog::hintsFor(const PluginEntry &e) const
{
	QStringList hints;
	if (e.leftover)
		return {T("Hint.Leftover")};
	if (!e.missingLibraries.isEmpty())
		hints << T("Hint.MissingLibraries").arg(e.missingLibraries.join(", "));
	if (!e.usage.isEmpty()) {
		QStringList places;
		for (auto it = e.usage.begin(); it != e.usage.end(); ++it) {
			QString place = it.key().startsWith("profile:") ? T("Usage.Profile").arg(it.key().mid(8))
									: it.key();
			places << QString("%1 (%2)").arg(place).arg(it.value());
		}
		places.sort(Qt::CaseInsensitive);
		hints << T("Hint.UsedIn").arg(places.join(", "));
	} else if (!e.types.isEmpty()) {
		hints << T("Hint.Unused");
	} else if (e.loaded || e.disabled) {
		hints << T("Hint.NoTypes");
	}
	if (offers_update(e) && obs_is_portable()) {
#ifdef _WIN32
		QString root = display_path(obs_install_root());
#else
		QString root;
#endif
		hints << (e.downloadIsInstaller ? T("Hint.PortableInstaller") : T("Hint.PortableZip")).arg(root);
	}
	if (e.disabled)
		hints << T("Hint.Disabled");
	else if (!e.loaded)
		hints << T("Hint.NotLoaded");
	if (e.legacy)
		hints << T("Hint.Legacy");
	for (const QString &d : e.duplicates)
		hints << T("Hint.Duplicate").arg(display_path(d));
	if (e.archived)
		hints << T("Hint.Archived");
	if (e.releaseBeforeObsMajor && !checker_->obsLatest.isEmpty())
		hints << T("Hint.OldRelease").arg(checker_->obsLatest.section('.', 0, 0));
	if (e.status == PluginEntry::Status::Unknown)
		hints << T("Hint.NoRepo");
	if (e.status == PluginEntry::Status::Check && e.version.isEmpty())
		hints << T("Hint.NoVersion");
	if (!e.error.isEmpty())
		hints << text_or_key("Error." + e.error);
	if (!e.note.isEmpty())
		hints << text_or_key(e.note);
	return hints;
}

void CheckerDialog::refresh()
{
	QString installed = checker_->obsInstalled;
	QString latest = checker_->obsLatest;
	QString head = T("Header.Obs").arg(installed.toHtmlEscaped(), latest.isEmpty() ? "?" : latest.toHtmlEscaped());
	if (!latest.isEmpty() && compare_versions(latest, installed) > 0)
		head += " &nbsp; <b style='color:#f0a020'>" + T("Header.ObsUpdate") + "</b>";

	const PluginEntry *sel = selected();
	QString current = sel ? sel->name : QString();
	int updates = 0, oldPlace = 0, broken = 0;
	table_->setSortingEnabled(false);
	table_->clearContents();
	table_->setRowCount(int(checker_->plugins.size()));
	for (int i = 0; i < checker_->plugins.size(); i++) {
		const PluginEntry &e = checker_->plugins[i];
		Future future = future_of(e);
		updates += e.status == PluginEntry::Status::Update;
		oldPlace += future == Future::OldPlace;
		broken += future == Future::Broken;

		QString tip = e.name + "\n" + display_path(e.location);
		QString shown = e.displayName.isEmpty() ? e.name : e.displayName;
		if (e.self)
			shown += "  " + T("Self.Marker");
		const QString cells[kColumns] = {shown,
						 e.version.isEmpty() ? "–" : e.version,
						 e.latest.isEmpty() ? "–" : e.latest,
						 status_text(e.status),
						 usage_text(e),
						 future_text(future)};
		for (int c = 0; c < kColumns; c++) {
			auto item = new QTableWidgetItem(cells[c]);
			item->setToolTip(tip);
			if (c == 3) {
				item->setForeground(status_color(e.status));
				if (e.status == PluginEntry::Status::Update) {
					QFont f = item->font();
					f.setBold(true);
					item->setFont(f);
				}
			} else if (c == 5) {
				item->setForeground(future_color(future));
			} else if (c == 0 && e.self) {
				QFont f = item->font();
				f.setItalic(true);
				item->setFont(f);
			}
			table_->setItem(i, c, item);
		}
		table_->item(i, 0)->setData(Qt::UserRole, i);
	}
	table_->setSortingEnabled(true);
	fitColumns();
	for (int row = 0; row < table_->rowCount(); row++)
		if (auto item = table_->item(row, 0);
		    item && checker_->plugins[item->data(Qt::UserRole).toInt()].name == current)
			table_->selectRow(row);

	if (!checker_->plugins.isEmpty())
		head += "<br>" + T("Header.Summary")
					 .arg(count_text("Count.Plugins", checker_->plugins.size()))
					 .arg(count_text("Count.Updates", updates))
					 .arg(oldPlace)
					 .arg(broken);
	for (const PluginEntry &e : checker_->plugins) {
		if (e.self && e.status == PluginEntry::Status::Update && !e.releaseUrl.isEmpty()) {
			head += "<br><b style='color:#f0a020'>" + T("Header.SelfUpdate").arg(e.latest).toHtmlEscaped() +
				"</b> &nbsp;<a href='" + e.releaseUrl.toHtmlEscaped() + "'>" +
				T("Header.SelfUpdateLink").toHtmlEscaped() + "</a>";
		}
	}
	header_->setText(head);

	if (checker_->plugins.isEmpty() && !checker_->running())
		footer_->setText(T("Footer.NoPlugins"));
	else if (checker_->running())
		footer_->setText(T("Footer.Checking"));
	else if (checker_->rateLimited)
		footer_->setText(T("Footer.RateLimited"));
	else if (checker_->lastCheck.isValid())
		footer_->setText(T("Footer.LastCheck").arg(checker_->lastCheck.toString("HH:mm")));
	updateButtons();
	updateDetails();
}

void CheckerDialog::mapRepo()
{
	auto e = selected();
	if (!e)
		return;
	QString name = e->name;
	QString current = e->repo.isEmpty() ? e->homepage : e->repo;
	bool ok = false;
	QString value = QInputDialog::getText(this, T("Map.Title"), T("Map.Label").arg(name), QLineEdit::Normal,
					      current, &ok);
	if (!ok)
		return;
	checker_->setRepo(name, value);
	checker_->start(false);
}

void CheckerDialog::showFiles()
{
	if (!selected())
		return;
	/* a copy: a re-check can replace the plugin list while this dialog is open */
	const PluginEntry entry = *selected();
	const PluginEntry *e = &entry;

	QDialog dialog(this);
	dialog.setWindowTitle(T("Files.Title").arg(e->displayName.isEmpty() ? e->name : e->displayName));
	dialog.resize(820, 420);

	auto list = new QListWidget(&dialog);
	qint64 total = 0;
	for (const QString &path : e->files) {
		QFileInfo fi(path);
		total += fi.size();
		QString text = display_path(path) + "   (" + QLocale().formattedDataSize(fi.size()) + ")";
		if (e->sharedFiles.contains(path))
			text += "   – " + T("Files.Shared");
		auto item = new QListWidgetItem(text, list);
		item->setData(Qt::UserRole, path);
	}

	auto summary = new QLabel(T("Files.Summary").arg(e->files.size()).arg(QLocale().formattedDataSize(total)),
				  &dialog);
	auto note = new QLabel(T("Files.Note"), &dialog);
	note->setWordWrap(true);
	note->setStyleSheet("color: gray;");

	auto buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
	auto copy = buttons->addButton(T("Files.Copy"), QDialogButtonBox::ActionRole);
	auto open = buttons->addButton(T("Button.Reveal"), QDialogButtonBox::ActionRole);
	open->setEnabled(false);
	auto remove = buttons->addButton(T("Files.Remove"), QDialogButtonBox::DestructiveRole);
	/* The checker cannot remove itself while it runs: use the uninstaller or delete the files by hand. */
	remove->setEnabled(!e->files.isEmpty() && !e->self);
	if (e->self)
		remove->setToolTip(T("Files.RemoveSelf"));
	/* the checker's own config folder holds the backups of all updates and removals */
	QPushButton *clean = nullptr;
	if (e->self)
		clean = buttons->addButton(T("Backups.Button"), QDialogButtonBox::ActionRole);

	auto layout = new QVBoxLayout(&dialog);
	layout->addWidget(summary);
	layout->addWidget(list, 1);
	layout->addWidget(note);
	layout->addWidget(buttons);

	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	connect(list, &QListWidget::currentItemChanged, &dialog,
		[open](QListWidgetItem *item) { open->setEnabled(item != nullptr); });
	connect(open, &QPushButton::clicked, &dialog, [list]() {
		if (auto item = list->currentItem())
			QDesktopServices::openUrl(
				QUrl::fromLocalFile(QFileInfo(item->data(Qt::UserRole).toString()).absolutePath()));
	});
	connect(copy, &QPushButton::clicked, &dialog, [e]() {
		QStringList lines;
		for (const QString &path : e->files)
			lines << QDir::toNativeSeparators(path);
		QApplication::clipboard()->setText(lines.join("\n"));
	});
	connect(remove, &QPushButton::clicked, &dialog, [this, &entry, &dialog]() {
		if (removePlugin(entry))
			dialog.accept();
	});
	if (clean)
		connect(clean, &QPushButton::clicked, &dialog, [this, &dialog]() {
			cleanBackups();
			dialog.accept();
		});
	dialog.exec();
}

static QString module_config_path(const char *name)
{
	char *path = obs_module_config_path(name);
	QString dir = QDir::cleanPath(QFileInfo(QString::fromUtf8(path)).absoluteFilePath());
	bfree(path);
	return dir;
}

void CheckerDialog::cleanBackups()
{
	const QString updatesRoot = module_config_path("updates");
	const QString removedRoot = module_config_path("removed");
	const QList<BackupJob> jobs = list_backups(updatesRoot, removedRoot);

	QDialog dialog(this);
	dialog.setWindowTitle(T("Backups.Title"));
	dialog.resize(760, 420);

	auto list = new QListWidget(&dialog);
	qint64 total = 0;
	for (int i = 0; i < jobs.size(); i++) {
		const BackupJob &job = jobs[i];
		total += job.bytes;
		QString text = QLocale().toString(job.when, QLocale::ShortFormat) + "   " + job.plugin + "   " +
			       T(job.kind == "update" ? "Backups.Update" : "Backups.Remove") + "   (" +
			       QLocale().formattedDataSize(job.bytes) + ")";
		auto item = new QListWidgetItem(list);
		item->setData(Qt::UserRole, i);
		if (job.pending) {
			item->setText(text + "   – " + T("Backups.Pending"));
			item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable & ~Qt::ItemIsEnabled);
		} else {
			item->setText(text);
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(Qt::Unchecked);
		}
	}

	auto summary = new QLabel(jobs.isEmpty() ? T("Backups.None")
						 : T("Backups.Summary").arg(jobs.size()).arg(QLocale().formattedDataSize(total)),
				  &dialog);
	auto note = new QLabel(T("Backups.Note"), &dialog);
	note->setWordWrap(true);
	note->setStyleSheet("color: gray;");

	auto buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
	auto older = buttons->addButton(T("Backups.SelectOlder"), QDialogButtonBox::ActionRole);
	auto all = buttons->addButton(T("Backups.SelectAll"), QDialogButtonBox::ActionRole);
	auto del = buttons->addButton(T("Backups.Delete"), QDialogButtonBox::DestructiveRole);
	older->setEnabled(!jobs.isEmpty());
	all->setEnabled(!jobs.isEmpty());

	auto layout = new QVBoxLayout(&dialog);
	layout->addWidget(summary);
	layout->addWidget(list, 1);
	layout->addWidget(note);
	layout->addWidget(buttons);

	auto checked = [list]() {
		QList<int> rows;
		for (int r = 0; r < list->count(); r++)
			if (list->item(r)->checkState() == Qt::Checked && (list->item(r)->flags() & Qt::ItemIsEnabled))
				rows << list->item(r)->data(Qt::UserRole).toInt();
		return rows;
	};
	auto updateDelete = [del, checked]() { del->setEnabled(!checked().isEmpty()); };
	updateDelete();

	connect(list, &QListWidget::itemChanged, &dialog, updateDelete);
	/* keeps the newest backup of each plugin, so the last change can still be undone */
	connect(older, &QPushButton::clicked, &dialog, [list, &jobs]() {
		QSet<QString> newest;
		for (int r = 0; r < list->count(); r++) {
			auto item = list->item(r);
			const BackupJob &job = jobs[item->data(Qt::UserRole).toInt()];
			if (!(item->flags() & Qt::ItemIsEnabled))
				continue;
			bool keep = !newest.contains(job.plugin);
			newest.insert(job.plugin);
			item->setCheckState(keep ? Qt::Unchecked : Qt::Checked);
		}
	});
	connect(all, &QPushButton::clicked, &dialog, [list]() {
		for (int r = 0; r < list->count(); r++)
			if (list->item(r)->flags() & Qt::ItemIsEnabled)
				list->item(r)->setCheckState(Qt::Checked);
	});
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	connect(del, &QPushButton::clicked, &dialog, [&]() {
		const QList<int> rows = checked();
		qint64 bytes = 0;
		for (int i : rows)
			bytes += jobs[i].bytes;
		if (QMessageBox::question(&dialog, T("Backups.Title"),
					  T("Backups.Confirm").arg(rows.size()).arg(QLocale().formattedDataSize(bytes))) !=
		    QMessageBox::Yes)
			return;
		int failed = 0;
		for (int i : rows)
			failed += !delete_backup(jobs[i], updatesRoot, removedRoot);
		if (failed)
			QMessageBox::warning(&dialog, T("Backups.Title"), T("Backups.Failed").arg(failed));
		dialog.accept();
		/* the file list of the checker's own entry shows the backups: scan again (GitHub answers are cached) */
		checker_->start(false);
	});
	dialog.exec();
}

bool CheckerDialog::removePlugin(const PluginEntry &e)
{
	/* Only the plugin's own files inside OBS's folders; helper DLLs other plugins need stay. */
	QStringList roots = removable_roots();
	auto in_roots = [&roots](const QString &path) {
		QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
		for (const QString &root : roots)
			if (clean.startsWith(QDir::cleanPath(root) + "/", Qt::CaseInsensitive))
				return true;
		return false;
	};
	QStringList move, keep;
	for (const QString &path : e.files)
		(in_roots(path) && !e.sharedFiles.contains(path) ? move : keep).append(path);
	if (move.isEmpty())
		return false;

	char *configPath = obs_module_config_path("removed");
	QString backupRoot = QDir::cleanPath(QFileInfo(QString::fromUtf8(configPath)).absoluteFilePath());
	bfree(configPath);

	QString name = e.displayName.isEmpty() ? e.name : e.displayName;
	QString text = T("Remove.Confirm").arg(name).arg(move.size()).arg(display_path(backupRoot)).arg(kRestoreScript);
	if (!keep.isEmpty())
		text += "\n\n" + T("Remove.Kept").arg(keep.size());
	if (QMessageBox::question(this, T("Remove.Title"), text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) !=
	    QMessageBox::Yes)
		return false;

	RemovalPlan plan;
	plan.plugin = e.name;
	plan.files = move;
	plan.backupRoot = backupRoot;
	if (QFileInfo(e.location).isDir())
		plan.cleanup << e.location;
	if (!e.dataPath.isEmpty())
		plan.cleanup << e.dataPath;
	plan.cleanup << obs_user_config_dir() + "/plugin_config/" + e.name;
	plan.cleanup.erase(std::remove_if(plan.cleanup.begin(), plan.cleanup.end(),
					  [&in_roots](const QString &dir) { return !in_roots(dir); }),
			   plan.cleanup.end());

	RemovalResult result = schedule_removal(plan);
	if (!result.ok) {
		QMessageBox::warning(this, T("Remove.Title"), T("Remove.Failed"));
		return false;
	}
	QString done = T("Remove.Scheduled").arg(name, display_path(result.backupDir));
#ifdef _WIN32
	done += "\n\n" + T("Remove.AdminHint");
#endif
	QMessageBox::information(this, T("Remove.Title"), done);
	return true;
}

void CheckerDialog::updateDetails()
{
	auto e = selected();
	if (!e) {
		details_->setText("<span style='color:gray'>" + T("Details.Select").toHtmlEscaped() + "</span>");
		return;
	}
	QString html = "<b>" + (e->displayName.isEmpty() ? e->name : e->displayName).toHtmlEscaped() + "</b>";
	if (!e->displayName.isEmpty() && e->displayName != e->name)
		html += " &nbsp;<span style='color:gray'>" + e->name.toHtmlEscaped() + "</span>";
	html += "<br><span style='color:gray'>" + display_path(e->location).toHtmlEscaped();
	if (!e->publishedAt.isEmpty())
		html += " &nbsp;·&nbsp; " + T("Details.Released").arg(e->publishedAt.left(10)).toHtmlEscaped();
	if (!e->downloadUrl.isEmpty()) {
		QString file = QUrl(e->downloadUrl).fileName();
		html += "<br>" + T(e->downloadIsInstaller ? "Details.DownloadInstaller" : "Details.DownloadArchive")
					 .arg(file)
					 .toHtmlEscaped();
	}
	html += "</span>";
	QStringList hints = hintsFor(*e);
	if (!hints.isEmpty()) {
		html += "<ul style='margin:4px 0 0 -20px'>";
		for (const QString &h : hints)
			html += "<li>" + h.toHtmlEscaped() + "</li>";
		html += "</ul>";
	}
	details_->setText(html);
	/* The label sits in a QScrollArea (label -> viewport -> scroll area): start each plugin at the top. */
	if (auto viewport = details_->parentWidget())
		if (auto scroll = qobject_cast<QScrollArea *>(viewport->parentWidget()))
			scroll->verticalScrollBar()->setValue(0);
}

void CheckerDialog::updatePlugin(const PluginEntry &e)
{
	QUrl url(e.downloadUrl);
	if (url.scheme() != "https" || url.host() != "github.com")
		return;
	downloading_ = true;
	updateButtons();
	footer_->setText(T("Update.Downloading").arg(url.fileName()));

	/* the dialog may be closed and a re-check may replace the plugin list meanwhile */
	QPointer<CheckerDialog> self(this);
	const PluginEntry entry = e;
	checker_->download(e.downloadUrl, [self, entry](int status, const QByteArray &body) {
		if (!self)
			return;
		self->downloading_ = false;
		self->refresh();
		if (status != 200 || body.isEmpty()) {
			QMessageBox::warning(self, T("Update.Title"), T("Update.DownloadFailed"));
			return;
		}
		self->confirmUpdate(entry, body);
	});
}

void CheckerDialog::confirmUpdate(const PluginEntry &e, const QByteArray &zip)
{
	QString name = e.displayName.isEmpty() ? e.name : e.displayName;
	QString digestLine;
	if (e.downloadDigest.isEmpty()) {
		digestLine = T("Update.NoDigest");
	} else if (!digest_matches(zip, e.downloadDigest)) {
		QMessageBox::warning(this, T("Update.Title"), T("Update.DigestMismatch"));
		return;
	} else {
		digestLine = T("Update.DigestOk");
	}

	char *configPath = obs_module_config_path("updates");
	QString jobRoot = QDir::cleanPath(QFileInfo(QString::fromUtf8(configPath)).absoluteFilePath());
	bfree(configPath);

	UpdatePlan plan;
	QString why = prepare_update(e, zip, jobRoot, plan);
	if (!why.isEmpty()) {
		QMessageBox::warning(this, T("Update.Title"), text_or_key(why));
		return;
	}

	QDialog dialog(this);
	dialog.setWindowTitle(T("Update.Title"));
	dialog.resize(820, 460);
	int replaced = 0;
	auto list = new QListWidget(&dialog);
	for (const UpdateFile &f : plan.files) {
		replaced += f.replaces;
		new QListWidgetItem((f.replaces ? T("Update.Replaced") : T("Update.New")) + "   " + display_path(f.target),
				    list);
	}
	for (const QString &ignored : plan.ignored)
		new QListWidgetItem(T("Update.Ignored") + "   " + QDir::toNativeSeparators(ignored), list);

	auto summary = new QLabel(T("Update.Summary")
					  .arg(name, e.version.isEmpty() ? "?" : e.version, e.latest)
					  .arg(replaced)
					  .arg(plan.files.size() - replaced)
					  .arg(plan.ignored.size()),
				  &dialog);
	summary->setWordWrap(true);
	auto note = new QLabel(digestLine + "\n" + T("Update.Note").arg(display_path(plan.jobDir)), &dialog);
	note->setWordWrap(true);
	note->setStyleSheet("color: gray;");

	auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
	buttons->addButton(T("Update.Install"), QDialogButtonBox::AcceptRole);
	auto layout = new QVBoxLayout(&dialog);
	layout->addWidget(summary);
	layout->addWidget(list, 1);
	layout->addWidget(note);
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	if (dialog.exec() != QDialog::Accepted) {
		QDir(plan.jobDir).removeRecursively();
		return;
	}
	RemovalResult result = schedule_update(plan);
	if (!result.ok) {
		QDir(plan.jobDir).removeRecursively();
		QMessageBox::warning(this, T("Update.Title"), T("Update.Failed"));
		return;
	}
	QMessageBox::information(this, T("Update.Title"), T("Update.Scheduled").arg(name, display_path(result.backupDir)));
}
