#pragma once

#include "plugin-remover.hpp"
#include "plugin-scanner.hpp"

#include <QByteArray>

/* Checks a release ZIP against the digest GitHub lists for it ("sha256:<hex>"). */
bool digest_matches(const QByteArray &data, const QString &digest);

/* Extracts a release ZIP into a new job folder under jobRoot and maps the plugin's files in it onto the
 * installed plugin: its binary folder and its data folder, nothing else. Returns an empty string, or a
 * locale key saying why the ZIP cannot be used; the job folder is removed again in that case. */
QString prepare_update(const PluginEntry &e, const QByteArray &zip, const QString &jobRoot, UpdatePlan &plan);
