#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

/* Runs HTTPS GET requests with libcurl on one background thread.
 * Qt's own TLS backend is not shipped with OBS on macOS, so QNetworkAccessManager cannot do HTTPS there. */
class HttpWorker {
public:
	struct Response {
		int status = 0; // 0 = network error
		QByteArray body;
		bool rateLimited = false;
	};
	/* Called on the worker thread. */
	using Done = std::function<void(const Response &)>;

	HttpWorker();
	~HttpWorker();

	void get(const QString &url, Done done);
	/* A release file: larger limit, longer timeout. */
	void download(const QString &url, Done done);

private:
	struct Job {
		QString url;
		Done done;
		bool file = false;
	};

	void run();

	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<Job> queue_;
	std::atomic<bool> stop_{false};
};
