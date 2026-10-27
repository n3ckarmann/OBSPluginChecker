#include "update-checker.hpp"
#include "http-worker.hpp"

#include <obs-module.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSaveFile>

static const char *kObsRepo = "obsproject/obs-studio";
static const qint64 kCacheSeconds = 6 * 3600;

static QString config_file(const char *name)
{
	char *path = obs_module_config_path(name);
	QString result = QString::fromUtf8(path);
	bfree(path);
	return result;
}

static QJsonObject read_object(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return {};
	return QJsonDocument::fromJson(f.readAll()).object();
}

static void write_object(const QString &path, const QJsonObject &obj)
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	QSaveFile f(path);
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(obj).toJson());
		f.commit();
	}
}

/* Downloads built for another CPU: ARM builds on x64 and 32-bit Windows builds. */
static bool wrong_arch(const QString &name)
{
#if defined(__x86_64__) || defined(_M_X64)
	static const QRegularExpression arm("(arm64|aarch64)", QRegularExpression::CaseInsensitiveOption);
	if (arm.match(name).hasMatch())
		return true;
#endif
#ifdef _WIN32
	static const QRegularExpression x86("(^|[^a-z0-9])(x86|win32|32bit|ia32)([^a-z0-9_]|$)",
					    QRegularExpression::CaseInsensitiveOption);
	static const QRegularExpression x64("(x64|x86_64|amd64|win64|64bit)", QRegularExpression::CaseInsensitiveOption);
	if (x86.match(name).hasMatch() && !x64.match(name).hasMatch())
		return true;
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
	/* x86_64-only builds; "universal" builds contain both */
	static const QRegularExpression intel("(x86_64|x64|amd64|intel)", QRegularExpression::CaseInsensitiveOption);
	static const QRegularExpression universal("universal", QRegularExpression::CaseInsensitiveOption);
	if (intel.match(name).hasMatch() && !universal.match(name).hasMatch())
		return true;
#endif
	return false;
}

/* Picks the release download that fits this operating system. A portable OBS gets a ZIP if there is
 * one: installers only know the standard OBS location. */
static QJsonObject pick_asset(const QJsonArray &assets, bool portable)
{
#if defined(__APPLE__)
	(void)portable;
	const QStringList prefs = {"mac.*universal.*\\.pkg$", "mac.*\\.pkg$", "\\.pkg$", "mac.*\\.(zip|tar\\.xz|dmg)$"};
#elif defined(_WIN32)
	const QStringList installer = {"win.*\\.(exe|msi)$", "\\.(exe|msi)$"};
	/* A portable OBS needs the flat layout (obs-plugins/64bit + data/obs-plugins): "portable" builds
	 * first, then plain Windows ZIPs, then "legacy" ones (often the ProgramData layout, which the
	 * updater can map too), "programdata" ZIPs last. */
	const QStringList archive = {"portable.*\\.zip$",
				     "^(?!.*(programdata|legacy)).*win.*\\.zip$",
				     "^(?!.*(programdata|legacy)).*x64.*\\.zip$",
				     "^(?!.*programdata).*win.*\\.zip$",
				     "win.*\\.zip$"};
	const QStringList prefs = portable ? archive + installer : installer + archive;
#else
	(void)portable;
	/* Some plugins ship two packages: "-legacy" for OBS before 33 (old plugin folders) and one for the
	 * new layout, which refuses to install on older OBS. */
	const bool newLayout = (obs_get_version() >> 24) >= 33;
	const QString deb = newLayout ? "^(?!.*legacy).*(ubuntu|linux).*\\.deb$" : "(ubuntu|linux).*legacy.*\\.deb$";
	const QStringList prefs = {deb, "(ubuntu|linux).*\\.deb$", "\\.deb$", "(ubuntu|linux).*\\.(tar\\.(xz|gz)(\\.zip)?|zip)$",
				   "\\.flatpak$"};
#endif
	for (const QString &pattern : prefs) {
		QRegularExpression re(pattern, QRegularExpression::CaseInsensitiveOption);
		for (const QJsonValue &a : assets) {
			QString name = a["name"].toString();
			if (re.match(name).hasMatch() && !name.endsWith(".sig") && !name.endsWith(".sha256") &&
			    !wrong_arch(name))
				return a.toObject();
		}
	}
	return {};
}

static bool is_installer(const QString &url)
{
	static const QRegularExpression re("\\.(exe|msi|pkg|deb)$", QRegularExpression::CaseInsensitiveOption);
	return re.match(url).hasMatch();
}

UpdateChecker::UpdateChecker() : http_(new HttpWorker())
{
	loadCache();
}

UpdateChecker::~UpdateChecker()
{
	/* Joins the worker thread before context_ goes away, so no reply can arrive afterwards. */
	delete http_;
	saveCache();
}

int UpdateChecker::updateCount() const
{
	int n = 0;
	for (const PluginEntry &e : plugins)
		if (e.status == PluginEntry::Status::Update)
			n++;
	return n;
}

void UpdateChecker::notify()
{
	if (changed)
		changed();
}

void UpdateChecker::loadCache()
{
	cache_ = read_object(config_file("github-cache.json"));
}

void UpdateChecker::saveCache()
{
	write_object(config_file("github-cache.json"), cache_);
}

void UpdateChecker::setRepo(const QString &plugin, const QString &repoOrUrl)
{
	QString path = config_file("repo-map.json");
	QJsonObject map = read_object(path);
	if (repoOrUrl.trimmed().isEmpty())
		map.remove(plugin);
	else
		map[plugin] = repoOrUrl.trimmed();
	write_object(path, map);
}

void UpdateChecker::applyMappings()
{
	char *knownPath = obs_module_file("known-plugins.json");
	QJsonObject known = read_object(QString::fromUtf8(knownPath));
	bfree(knownPath);
	QJsonObject user = read_object(config_file("repo-map.json"));

	static const QRegularExpression githubUrl("^(?:https?://)?github\\.com/([^/]+/[^/#?]+?)(?:\\.git)?/?$");
	static const QRegularExpression ownerRepo("^[\\w.-]+/[\\w.-]+$");

	for (PluginEntry &e : plugins) {
		if (known.contains(e.name)) {
			QJsonObject k = known[e.name].toObject();
			if (e.repo.isEmpty())
				e.repo = k["repo"].toString();
			if (e.homepage.isEmpty())
				e.homepage = k["homepage"].toString();
			e.note = k["note"].toString();
		}
		QString mapped = user[e.name].toString().trimmed();
		if (mapped.isEmpty())
			continue;
		auto m = githubUrl.match(mapped);
		if (m.hasMatch()) {
			e.repo = m.captured(1);
		} else if (ownerRepo.match(mapped).hasMatch()) {
			e.repo = mapped;
		} else {
			e.repo.clear();
			e.homepage = mapped;
		}
	}
}

void UpdateChecker::get(const QString &apiPath, Callback cb)
{
	pending_++;
	int gen = generation_;

	QJsonObject hit = cache_[apiPath].toObject();
	qint64 age = QDateTime::currentSecsSinceEpoch() - qint64(hit["t"].toDouble());
	if (!hit.isEmpty() && (!force_ || rateLimited) && age < kCacheSeconds) {
		QJsonDocument doc = QJsonDocument::fromJson(hit["body"].toString().toUtf8());
		cb(hit["status"].toInt(), doc);
		finishOne();
		return;
	}

	auto handle = [this, apiPath, cb, hit, gen](const HttpWorker::Response &res) {
		if (gen != generation_)
			return;
		int status = res.status;
		const QByteArray &body = res.body;

		if ((status == 403 && res.rateLimited) || status == 429)
			rateLimited = true;
		if (status == 200 || status == 404) {
			QJsonObject entry;
			entry["t"] = double(QDateTime::currentSecsSinceEpoch());
			entry["status"] = status;
			entry["body"] = QString::fromUtf8(body);
			cache_[apiPath] = entry;
			cb(status, QJsonDocument::fromJson(body));
		} else if (!hit.isEmpty()) {
			/* Offline or rate-limited: fall back to the last known answer. */
			cb(hit["status"].toInt(), QJsonDocument::fromJson(hit["body"].toString().toUtf8()));
		} else {
			cb(status, {});
		}
		finishOne();
	};
	http_->get("https://api.github.com" + apiPath, [this, handle](const HttpWorker::Response &res) {
		/* Back to the Qt main thread. */
		QMetaObject::invokeMethod(&context_, [handle, res]() { handle(res); }, Qt::QueuedConnection);
	});
}

void UpdateChecker::download(const QString &url, std::function<void(int, const QByteArray &)> done)
{
	http_->download(url, [this, done](const HttpWorker::Response &res) {
		QMetaObject::invokeMethod(&context_, [done, res]() { done(res.status, res.body); },
					  Qt::QueuedConnection);
	});
}

void UpdateChecker::finishOne()
{
	if (--pending_ > 0) {
		notify();
		return;
	}

	for (PluginEntry &e : plugins) {
		QDateTime published = QDateTime::fromString(e.publishedAt, Qt::ISODate);
		e.releaseBeforeObsMajor = obsMajorDate_.isValid() && published.isValid() && published < obsMajorDate_;
		if (e.status == PluginEntry::Status::Checking)
			e.status = PluginEntry::Status::Error;
	}
	lastCheck = QDateTime::currentDateTime();
	saveCache();
	notify();
}

void UpdateChecker::start(bool force)
{
	if (running())
		return;
	generation_++;
	force_ = force;
	rateLimited = false;
	obsInstalled = QString::fromUtf8(obs_get_version_string());
	plugins = scan_plugins();
	applyMappings();

	/* Keep the counter above zero until all requests are queued. */
	pending_++;

	get(QString("/repos/%1/releases/latest").arg(kObsRepo), [this](int status, const QJsonDocument &doc) {
		if (status != 200)
			return;
		obsLatest = doc["tag_name"].toString();
		QString major = obsLatest.section('.', 0, 0);
		get(QString("/repos/%1/releases/tags/%2.0.0").arg(kObsRepo, major),
		    [this](int s, const QJsonDocument &d) {
			    if (s == 200)
				    obsMajorDate_ = QDateTime::fromString(d["published_at"].toString(), Qt::ISODate);
		    });
	});

	for (int i = 0; i < plugins.size(); i++)
		checkPlugin(i);

	notify();
	finishOne();
}

void UpdateChecker::applyRelease(PluginEntry &e, const QJsonObject &release)
{
	e.latest = release["tag_name"].toString();
	e.publishedAt = release["published_at"].toString();
	e.releaseUrl = release["html_url"].toString();
	QJsonObject asset = pick_asset(release["assets"].toArray(), obs_is_portable());
	e.downloadUrl = asset["browser_download_url"].toString();
	e.downloadDigest = asset["digest"].toString(); // "sha256:<hex>", only on newer uploads
	e.downloadIsInstaller = is_installer(e.downloadUrl);
	int cmp = compare_versions(e.latest, e.version);
	if (!has_version_number(e.version) || !has_version_number(e.latest))
		e.status = PluginEntry::Status::Check;
	else
		e.status = cmp > 0 ? PluginEntry::Status::Update : PluginEntry::Status::UpToDate;
}

void UpdateChecker::checkPlugin(int index)
{
	PluginEntry &e = plugins[index];
	if (e.leftover) {
		e.status = PluginEntry::Status::Unknown;
		return;
	}
	if (e.repo.isEmpty()) {
		e.status = e.homepage.isEmpty() ? PluginEntry::Status::Unknown : PluginEntry::Status::Manual;
		return;
	}
	e.status = PluginEntry::Status::Checking;
	QString repo = e.repo;

	get("/repos/" + repo, [this, index](int status, const QJsonDocument &doc) {
		if (status == 200)
			plugins[index].archived = doc["archived"].toBool();
	});

	get("/repos/" + repo + "/releases/latest", [this, index, repo](int status, const QJsonDocument &doc) {
		PluginEntry &p = plugins[index];
		if (status == 200) {
			applyRelease(p, doc.object());
			return;
		}
		if (status != 404) {
			p.status = PluginEntry::Status::Error;
			p.error = rateLimited ? "RateLimited" : "Unreachable";
			return;
		}
		/* No "latest" release: the repo may only have pre-releases or tags. */
		get("/repos/" + repo + "/releases?per_page=5", [this, index, repo](int s, const QJsonDocument &d) {
			PluginEntry &q = plugins[index];
			if (s == 200 && !d.array().isEmpty()) {
				applyRelease(q, d.array().first().toObject());
				return;
			}
			if (s == 404) {
				q.status = PluginEntry::Status::Error;
				q.error = "RepoNotFound";
				return;
			}
			get("/repos/" + repo + "/tags?per_page=1", [this, index, repo](int t, const QJsonDocument &td) {
				PluginEntry &r = plugins[index];
				if (t == 200 && !td.array().isEmpty()) {
					QJsonObject fake;
					fake["tag_name"] = td.array().first()["name"].toString();
					fake["html_url"] = "https://github.com/" + repo + "/tags";
					applyRelease(r, fake);
				} else {
					r.status = PluginEntry::Status::Error;
					r.error = "NoReleases";
				}
			});
		});
	});
}
