#include "plugin-usage.hpp"

#include <obs-module.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

using enum_types_fn = bool (*)(size_t, const char **);
using type_module_fn = obs_module_t *(*)(const char *);

void *obs_symbol(const char *name)
{
#ifdef _WIN32
	HMODULE obs = GetModuleHandleW(L"obs.dll");
	return obs ? reinterpret_cast<void *>(GetProcAddress(obs, name)) : nullptr;
#else
	return dlsym(RTLD_DEFAULT, name);
#endif
}

QString module_key(obs_module_t *mod)
{
	QString name = QString::fromUtf8(obs_get_module_file_name(mod));
	int dot = name.lastIndexOf('.');
	return dot > 0 ? name.left(dot) : name;
}

void add_types(QHash<QString, QStringList> &out, enum_types_fn enumTypes, const char *lookup)
{
	auto getModule = reinterpret_cast<type_module_fn>(obs_symbol(lookup));
	if (!getModule)
		return; // OBS 31 and older cannot tell
	const char *id = nullptr;
	for (size_t i = 0; enumTypes(i, &id); i++) {
		if (obs_module_t *mod = id ? getModule(id) : nullptr)
			out[module_key(mod)].append(QString::fromUtf8(id));
	}
}

void count_object(const QJsonObject &o, const QString &where, QHash<QString, QHash<QString, int>> &usage);

void count_value(const QJsonValue &v, const QString &where, QHash<QString, QHash<QString, int>> &usage)
{
	if (v.isObject()) {
		count_object(v.toObject(), where, usage);
	} else if (v.isArray()) {
		for (const QJsonValue &item : v.toArray())
			count_value(item, where, usage);
	}
}

/* Sources, filters and transitions are saved as objects with an "id" plus "settings" or "versioned_id".
 * Requiring both keeps unrelated "id" keys inside plugin settings out. */
void count_object(const QJsonObject &o, const QString &where, QHash<QString, QHash<QString, int>> &usage)
{
	if (o["id"].isString() && (o.contains("settings") || o.contains("versioned_id"))) {
		QSet<QString> ids = {o["id"].toString()};
		if (o["versioned_id"].isString())
			ids.insert(o["versioned_id"].toString());
		for (const QString &id : ids)
			usage[id][where]++;
	}
	for (auto it = o.begin(); it != o.end(); ++it)
		count_value(it.value(), where, usage);
}

} // namespace

QHash<QString, QStringList> module_types(const QString &configDir)
{
	QHash<QString, QStringList> out;
	add_types(out, obs_enum_source_types, "obs_source_get_module");
	add_types(out, obs_enum_output_types, "obs_output_get_module");
	add_types(out, obs_enum_encoder_types, "obs_encoder_get_module");
	add_types(out, obs_enum_service_types, "obs_service_get_module");

	QFile f(configDir + "/plugin_manager/modules.json");
	if (f.open(QIODevice::ReadOnly)) {
		for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).array()) {
			QJsonObject o = v.toObject();
			QString name = o["module_name"].toString();
			if (name.isEmpty() || out.contains(name))
				continue;
			for (const char *key : {"sources", "outputs", "encoders", "services"})
				for (const QJsonValue &t : o[key].toArray())
					out[name].append(t.toString());
		}
	}
	for (QStringList &types : out)
		types.removeDuplicates();
	return out;
}

QHash<QString, QHash<QString, int>> type_usage(const QString &configDir)
{
	QHash<QString, QHash<QString, int>> usage;

	QDir scenes(configDir + "/basic/scenes");
	for (const QFileInfo &fi : scenes.entryInfoList({"*.json"}, QDir::Files)) {
		QFile f(fi.absoluteFilePath());
		if (!f.open(QIODevice::ReadOnly))
			continue;
		QJsonObject doc = QJsonDocument::fromJson(f.readAll()).object();
		QString name = doc["name"].toString(fi.completeBaseName());
		count_object(doc, name, usage);
	}

	/* Encoders are picked per profile in basic.ini. Only keys that name an encoder are read; service.json
	 * (stream key) and all other settings are never opened or kept. */
	QDir profiles(configDir + "/basic/profiles");
	for (const QFileInfo &dir : profiles.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		QString where = "profile:" + dir.fileName();
		QFile ini(dir.absoluteFilePath() + "/basic.ini");
		if (!ini.open(QIODevice::ReadOnly | QIODevice::Text))
			continue;
		QSet<QString> encoders;
		for (const QByteArray &line : ini.readAll().split('\n')) {
			int eq = line.indexOf('=');
			if (eq <= 0 || !line.left(eq).trimmed().endsWith("Encoder"))
				continue;
			QString value = QString::fromUtf8(line.mid(eq + 1).trimmed());
			if (!value.isEmpty())
				encoders.insert(value);
		}
		for (const QString &id : encoders)
			usage[id][where]++;
	}
	return usage;
}
