#pragma once

#include <QString>

namespace ve {

class Project;

// Persistence for projects.
//  * .veproj  - SQLite database (tables: meta, media, tracks, clips; clip
//               properties stored as JSON columns)
//  * .json    - plain JSON interchange format (same schema as Project::toJson)
namespace ProjectStore {

bool saveSqlite(const Project& project, const QString& path, QString* error = nullptr);
bool loadSqlite(Project& project, const QString& path, QString* error = nullptr);

bool saveJson(const Project& project, const QString& path, QString* error = nullptr);
bool loadJson(Project& project, const QString& path, QString* error = nullptr);

// Picks the format from the file extension.
bool save(const Project& project, const QString& path, QString* error = nullptr);
bool load(Project& project, const QString& path, QString* error = nullptr);

} // namespace ProjectStore
} // namespace ve
