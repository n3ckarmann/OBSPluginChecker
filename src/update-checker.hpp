#pragma once

#include "plugin-scanner.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>

#include <functional>

class HttpWorker;

class UpdateChecker {
public:
	UpdateChecker();
	~UpdateChecker();

	/* Scans the plugin folders and asks GitHub for the newest releases.
	 * force skips the one-hour response cache. */
	void start(bool force);
	bool running() const { return pending_ > 0; }

	void setRepo(const QString &plugin, const QString &repoOrUrl);

	QList<PluginEntry> plugins;
	QString obsInstalled;
	QString obsLatest;
	bool rateLimited = false;
	QDateTime lastCheck;

	int updateCount() const;

	/* Downloads a release file; done runs on the Qt main thread. */
	void download(const QString &url, std::function<void(int status, const QByteArray &body)> done);

	std::function<void()> changed;

private:
	using Callback = std::function<void(int status, const QJsonDocument &doc)>;

	void get(const QString &apiPath, Callback cb);
	void finishOne();
	void checkPlugin(int index);
	void applyRelease(PluginEntry &e, const QJsonObject &release);
	void applyMappings();
	void loadCache();
	void saveCache();
	void notify();

	HttpWorker *http_ = nullptr;
	QObject context_;
	QJsonObject cache_;
	QDateTime obsMajorDate_;
	bool force_ = false;
	int pending_ = 0;
	int generation_ = 0;
};
