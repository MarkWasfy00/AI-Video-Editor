#include "core/ProjectStore.h"

#include "core/Project.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

#include <sqlite3.h>

#include <memory>

namespace ve::ProjectStore {

namespace {

constexpr int kSchemaVersion = 3;

const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS media (
    id        TEXT PRIMARY KEY,
    path      TEXT NOT NULL,
    name      TEXT,
    info_json TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS tracks (
    id       TEXT PRIMARY KEY,
    position INTEGER NOT NULL,
    name     TEXT,
    muted    INTEGER NOT NULL DEFAULT 0,
    hidden   INTEGER NOT NULL DEFAULT 0,
    solo     INTEGER NOT NULL DEFAULT 0,
    gain_db  REAL NOT NULL DEFAULT 0,
    pan      REAL NOT NULL DEFAULT 0,
    kind     TEXT NOT NULL DEFAULT 'video'
);
CREATE TABLE IF NOT EXISTS clips (
    id          TEXT PRIMARY KEY,
    track_id    TEXT NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
    media_id    TEXT,
    start_us    INTEGER NOT NULL,
    in_us       INTEGER NOT NULL,
    duration_us INTEGER NOT NULL,
    data_json   TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS clips_by_track ON clips(track_id, start_us);
)SQL";

struct DbCloser {
    void operator()(sqlite3* db) const { sqlite3_close(db); }
};
using DbPtr = std::unique_ptr<sqlite3, DbCloser>;

struct StmtFinalizer {
    void operator()(sqlite3_stmt* s) const { sqlite3_finalize(s); }
};
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtFinalizer>;

bool fail(QString* error, const QString& msg)
{
    if (error) *error = msg;
    return false;
}

bool exec(sqlite3* db, const char* sql, QString* error)
{
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const QString msg = QString::fromUtf8(err ? err : "unknown SQLite error");
        sqlite3_free(err);
        return fail(error, msg);
    }
    return true;
}

StmtPtr prepare(sqlite3* db, const char* sql, QString* error)
{
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) {
        if (error) *error = QString::fromUtf8(sqlite3_errmsg(db));
        return {};
    }
    return StmtPtr(s);
}

void bindText(sqlite3_stmt* s, int idx, const QString& v)
{
    const QByteArray utf8 = v.toUtf8();
    sqlite3_bind_text(s, idx, utf8.constData(), static_cast<int>(utf8.size()), SQLITE_TRANSIENT);
}

QString colText(sqlite3_stmt* s, int idx)
{
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(s, idx));
    return p ? QString::fromUtf8(p, sqlite3_column_bytes(s, idx)) : QString();
}

QString compactJson(const QJsonObject& o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QJsonObject parseJson(const QString& s)
{
    return QJsonDocument::fromJson(s.toUtf8()).object();
}

DbPtr openDb(const QString& path, int flags, QString* error)
{
    sqlite3* raw = nullptr;
    const int rc = sqlite3_open_v2(QFileInfo(path).absoluteFilePath().toUtf8().constData(), &raw, flags, nullptr);
    DbPtr db(raw);
    if (rc != SQLITE_OK) {
        if (error) *error = QString::fromUtf8(raw ? sqlite3_errmsg(raw) : "cannot open database");
        return {};
    }
    sqlite3_busy_timeout(raw, 2000);
    return db;
}

} // namespace

bool saveSqlite(const Project& project, const QString& path, QString* error)
{
    DbPtr db = openDb(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, error);
    if (!db) return false;
    sqlite3* d = db.get();

    if (!exec(d, "PRAGMA foreign_keys = ON;", error)) return false;
    if (!exec(d, "BEGIN IMMEDIATE;", error)) return false;

    auto rollback = [&](QString* e) {
        sqlite3_exec(d, "ROLLBACK;", nullptr, nullptr, nullptr);
        Q_UNUSED(e);
        return false;
    };

    if (!exec(d, "DROP TABLE IF EXISTS clips; DROP TABLE IF EXISTS tracks; "
                 "DROP TABLE IF EXISTS media; DROP TABLE IF EXISTS meta;", error))
        return rollback(error);
    if (!exec(d, kSchema, error)) return rollback(error);

    // meta
    {
        auto st = prepare(d, "INSERT INTO meta(key, value) VALUES(?, ?);", error);
        if (!st) return rollback(error);
        const std::pair<QString, QString> rows[] = {
            {QStringLiteral("format"), QStringLiteral("video-editor-project")},
            {QStringLiteral("schema_version"), QString::number(kSchemaVersion)},
            {QStringLiteral("settings"), compactJson(project.settings().toJson())},
            {QStringLiteral("master"), compactJson(project.master().toJson())},
            {QStringLiteral("captions"), compactJson(project.captionStyle().toJson())},
            {QStringLiteral("transcripts"), [&project] {
                QJsonArray a;
                for (const QString& m : project.transcribedMedia())
                    if (const Transcript* t = project.transcript(m)) a.append(t->toJson());
                return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
            }()},
        };
        for (const auto& [k, v] : rows) {
            sqlite3_reset(st.get());
            bindText(st.get(), 1, k);
            bindText(st.get(), 2, v);
            if (sqlite3_step(st.get()) != SQLITE_DONE) return rollback(error);
        }
    }
    // media
    {
        auto st = prepare(d, "INSERT INTO media(id, path, name, info_json) VALUES(?, ?, ?, ?);", error);
        if (!st) return rollback(error);
        for (const auto& m : project.media()) {
            sqlite3_reset(st.get());
            bindText(st.get(), 1, m.id);
            bindText(st.get(), 2, m.path);
            bindText(st.get(), 3, m.name);
            bindText(st.get(), 4, compactJson(m.info.toJson()));
            if (sqlite3_step(st.get()) != SQLITE_DONE) {
                if (error) *error = QString::fromUtf8(sqlite3_errmsg(d));
                return rollback(error);
            }
        }
    }
    // tracks + clips
    {
        auto tst = prepare(d, "INSERT INTO tracks(id, position, name, muted, hidden, solo, gain_db, pan, kind) "
                              "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?);", error);
        auto cst = prepare(d, "INSERT INTO clips(id, track_id, media_id, start_us, in_us, duration_us, data_json) "
                              "VALUES(?, ?, ?, ?, ?, ?, ?);", error);
        if (!tst || !cst) return rollback(error);
        int pos = 0;
        for (const auto& t : project.tracks()) {
            sqlite3_reset(tst.get());
            bindText(tst.get(), 1, t.id);
            sqlite3_bind_int(tst.get(), 2, pos++);
            bindText(tst.get(), 3, t.name);
            sqlite3_bind_int(tst.get(), 4, t.muted ? 1 : 0);
            sqlite3_bind_int(tst.get(), 5, t.hidden ? 1 : 0);
            sqlite3_bind_int(tst.get(), 6, t.solo ? 1 : 0);
            sqlite3_bind_double(tst.get(), 7, t.gainDb);
            sqlite3_bind_double(tst.get(), 8, t.pan);
            bindText(tst.get(), 9, t.isAudio() ? QStringLiteral("audio") : QStringLiteral("video"));
            if (sqlite3_step(tst.get()) != SQLITE_DONE) {
                if (error) *error = QString::fromUtf8(sqlite3_errmsg(d));
                return rollback(error);
            }
            for (const auto& c : t.clips) {
                sqlite3_reset(cst.get());
                bindText(cst.get(), 1, c.id);
                bindText(cst.get(), 2, t.id);
                bindText(cst.get(), 3, c.mediaId);
                sqlite3_bind_int64(cst.get(), 4, c.start);
                sqlite3_bind_int64(cst.get(), 5, c.in);
                sqlite3_bind_int64(cst.get(), 6, c.duration);
                bindText(cst.get(), 7, compactJson(c.toJson()));
                if (sqlite3_step(cst.get()) != SQLITE_DONE) {
                    if (error) *error = QString::fromUtf8(sqlite3_errmsg(d));
                    return rollback(error);
                }
            }
        }
    }

    const QString pragma = QStringLiteral("PRAGMA user_version = %1;").arg(kSchemaVersion);
    if (!exec(d, pragma.toUtf8().constData(), error)) return rollback(error);
    return exec(d, "COMMIT;", error);
}

bool loadSqlite(Project& project, const QString& path, QString* error)
{
    if (!QFileInfo::exists(path)) return fail(error, QObject::tr("File does not exist: %1").arg(path));
    DbPtr db = openDb(path, SQLITE_OPEN_READONLY, error);
    if (!db) return false;
    sqlite3* d = db.get();

    QJsonObject root;
    QJsonObject settings;
    QJsonObject master;
    QJsonArray transcripts;
    QJsonObject captions;
    QString format;
    {
        auto st = prepare(d, "SELECT key, value FROM meta;", error);
        if (!st) return false;
        while (sqlite3_step(st.get()) == SQLITE_ROW) {
            const QString k = colText(st.get(), 0);
            if (k == QLatin1String("settings")) settings = parseJson(colText(st.get(), 1));
            else if (k == QLatin1String("format")) format = colText(st.get(), 1);
            else if (k == QLatin1String("master")) master = parseJson(colText(st.get(), 1));
            else if (k == QLatin1String("captions")) captions = parseJson(colText(st.get(), 1));
            else if (k == QLatin1String("transcripts"))
                transcripts = QJsonDocument::fromJson(colText(st.get(), 1).toUtf8()).array();
        }
    }
    if (format != QLatin1String("video-editor-project"))
        return fail(error, QObject::tr("Not a video editor project database"));

    QJsonArray media;
    {
        auto st = prepare(d, "SELECT id, path, name, info_json FROM media;", error);
        if (!st) return false;
        while (sqlite3_step(st.get()) == SQLITE_ROW) {
            media.append(QJsonObject{
                {"id", colText(st.get(), 0)},
                {"path", colText(st.get(), 1)},
                {"name", colText(st.get(), 2)},
                {"info", parseJson(colText(st.get(), 3))},
            });
        }
    }

    QJsonArray tracks;
    {
        // Schema 1 files have no mixer columns, schema 2 files no track kind
        // (Project then converts them to separate video/audio tracks).
        auto tst = prepare(d, "SELECT id, name, muted, hidden, solo, gain_db, pan, kind FROM tracks ORDER BY position;", nullptr);
        const bool hasKind = bool(tst);
        if (!tst) tst = prepare(d, "SELECT id, name, muted, hidden, solo, gain_db, pan FROM tracks ORDER BY position;", nullptr);
        const bool hasMixer = bool(tst);
        if (!hasMixer) tst = prepare(d, "SELECT id, name, muted, hidden FROM tracks ORDER BY position;", error);
        auto cst = prepare(d, "SELECT data_json, start_us, in_us, duration_us, media_id FROM clips "
                              "WHERE track_id = ? ORDER BY start_us;", error);
        if (!tst || !cst) return false;
        while (sqlite3_step(tst.get()) == SQLITE_ROW) {
            const QString tid = colText(tst.get(), 0);
            QJsonArray clips;
            sqlite3_reset(cst.get());
            bindText(cst.get(), 1, tid);
            while (sqlite3_step(cst.get()) == SQLITE_ROW) {
                QJsonObject c = parseJson(colText(cst.get(), 0));
                // Relational columns are authoritative for timing.
                c["start"] = static_cast<double>(sqlite3_column_int64(cst.get(), 1));
                c["in"] = static_cast<double>(sqlite3_column_int64(cst.get(), 2));
                c["duration"] = static_cast<double>(sqlite3_column_int64(cst.get(), 3));
                c["mediaId"] = colText(cst.get(), 4);
                clips.append(c);
            }
            QJsonObject track{
                {"id", tid},
                {"name", colText(tst.get(), 1)},
                {"muted", sqlite3_column_int(tst.get(), 2) != 0},
                {"hidden", sqlite3_column_int(tst.get(), 3) != 0},
                {"clips", clips},
            };
            if (hasMixer) {
                track["solo"] = sqlite3_column_int(tst.get(), 4) != 0;
                track["gainDb"] = sqlite3_column_double(tst.get(), 5);
                track["pan"] = sqlite3_column_double(tst.get(), 6);
            }
            if (hasKind) track["kind"] = colText(tst.get(), 7);
            tracks.append(track);
        }
    }

    root["format"] = format;
    root["version"] = 1;
    root["settings"] = settings;
    root["master"] = master;
    root["transcripts"] = transcripts;
    root["captions"] = captions;
    root["media"] = media;
    root["tracks"] = tracks;
    return project.fromJson(root, error);
}

bool saveJson(const Project& project, const QString& path, QString* error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return fail(error, f.errorString());
    f.write(QJsonDocument(project.toJson()).toJson(QJsonDocument::Indented));
    if (!f.commit()) return fail(error, f.errorString());
    return true;
}

bool loadJson(Project& project, const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail(error, f.errorString());
    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError) return fail(error, pe.errorString());
    return project.fromJson(doc.object(), error);
}

static bool isJsonPath(const QString& path)
{
    return path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive);
}

bool save(const Project& project, const QString& path, QString* error)
{
    return isJsonPath(path) ? saveJson(project, path, error) : saveSqlite(project, path, error);
}

bool load(Project& project, const QString& path, QString* error)
{
    const bool ok = isJsonPath(path) ? loadJson(project, path, error) : loadSqlite(project, path, error);
    if (ok) project.setFilePath(path);
    return ok;
}

} // namespace ve::ProjectStore
