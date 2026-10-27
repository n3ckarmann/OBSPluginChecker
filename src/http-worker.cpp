#include "http-worker.hpp"

#include <curl/curl.h>

/* GitHub answers are a few hundred KB at most, plugin ZIPs a few dozen MB; anything bigger is cut off
 * (returning less aborts). */
static constexpr qsizetype kMaxBody = 8 * 1024 * 1024;
static constexpr qsizetype kMaxFile = 256 * 1024 * 1024;

struct Sink {
	QByteArray *body;
	qsizetype max;
};

static size_t write_body(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto sink = static_cast<Sink *>(userdata);
	size_t len = size * nmemb;
	if (sink->body->size() + qsizetype(len) > sink->max)
		return 0;
	sink->body->append(ptr, qsizetype(len));
	return len;
}

static size_t read_header(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	QByteArray line(ptr, int(size * nmemb));
	if (line.toLower().startsWith("x-ratelimit-remaining:") && line.mid(22).trimmed() == "0")
		*static_cast<bool *>(userdata) = true;
	return size * nmemb;
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
static void restrict_protocols_legacy(CURL *curl)
{
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

static int on_progress(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	/* Non-zero aborts the transfer, so OBS does not wait for a slow request on exit. */
	return static_cast<std::atomic<bool> *>(clientp)->load() ? 1 : 0;
}

HttpWorker::HttpWorker()
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
	thread_ = std::thread([this]() { run(); });
}

HttpWorker::~HttpWorker()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = true;
		queue_.clear();
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
}

void HttpWorker::get(const QString &url, Done done)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		queue_.push_back({url, std::move(done)});
	}
	cv_.notify_one();
}

void HttpWorker::download(const QString &url, Done done)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		queue_.push_back({url, std::move(done), true});
	}
	cv_.notify_one();
}

void HttpWorker::run()
{
	CURL *curl = curl_easy_init();
	while (true) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
			if (stop_)
				break;
			job = std::move(queue_.front());
			queue_.pop_front();
		}

		Response res;
		if (curl) {
			QByteArray url = job.url.toUtf8();
			struct curl_slist *headers = nullptr;
			headers = curl_slist_append(headers, job.file ? "Accept: application/octet-stream"
								       : "Accept: application/vnd.github+json");
			Sink sink{&res.body, job.file ? kMaxFile : kMaxBody};

			curl_easy_reset(curl);
			curl_easy_setopt(curl, CURLOPT_URL, url.constData());
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
			curl_easy_setopt(curl, CURLOPT_USERAGENT, "obs-plugin-checker");
			curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // renamed repos answer with a redirect
			curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
			/* HTTPS only, also after redirects */
			if (curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https") != CURLE_OK ||
			    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https") != CURLE_OK)
				restrict_protocols_legacy(curl); // libcurl before 7.85 (macOS 12)
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
			curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, job.file ? 600L : 20L);
			curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
			curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, read_header);
			curl_easy_setopt(curl, CURLOPT_HEADERDATA, &res.rateLimited);
			curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
			curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
			curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &stop_);

			if (curl_easy_perform(curl) == CURLE_OK) {
				long code = 0;
				curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
				res.status = int(code);
			}
			curl_slist_free_all(headers);
		}
		if (stop_)
			break;
		job.done(res);
	}
	if (curl)
		curl_easy_cleanup(curl);
}
