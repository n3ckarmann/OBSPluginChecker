#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

/* Source, filter, transition, output, encoder and service types per module (module file name without
 * extension). Asks libobs where it can (OBS 32+) and fills in from the plugin manager's modules.json,
 * which also knows the types of disabled modules. */
QHash<QString, QStringList> module_types(const QString &configDir);

/* Where each type is used: type id -> (scene collection or profile name -> number of uses).
 * Reads all scene collections in basic/scenes and the encoder keys of each profile's basic.ini; profile
 * names carry a "profile:" prefix. Only type ids and counts are kept: no names, settings or URLs. */
QHash<QString, QHash<QString, int>> type_usage(const QString &configDir);
