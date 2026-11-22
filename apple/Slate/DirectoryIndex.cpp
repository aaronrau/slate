#include "DirectoryIndex.h"

#include <sqlite3.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
    return Statement(raw, sqlite3_finalize);
}
bool execute(sqlite3* db, const char* sql) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}
void bind(sqlite3_stmt* statement, int column, const std::string& value) {
    sqlite3_bind_text(statement, column, value.c_str(), -1, SQLITE_TRANSIENT);
}
std::string version(const struct stat& status) {
#ifdef __APPLE__
    const auto timestamp = status.st_mtimespec;
#else
    const auto timestamp = status.st_mtim;
#endif
    return std::to_string(status.st_dev) + "/" + std::to_string(status.st_ino) + "/" +
           std::to_string(timestamp.tv_sec) + "/" + std::to_string(timestamp.tv_nsec);
}

constexpr auto schema =
    "CREATE TABLE IF NOT EXISTS folders(id INTEGER PRIMARY KEY,path TEXT UNIQUE,version TEXT,n INTEGER);"
    "CREATE TABLE IF NOT EXISTS names(folder INTEGER,ordinal INTEGER,name TEXT,dir INTEGER,"
        "PRIMARY KEY(folder,ordinal)) WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS invalidation(id INTEGER PRIMARY KEY,epoch INTEGER);"
    "INSERT OR IGNORE INTO invalidation VALUES(1,0);";
}

struct SlateDirectory {
    sqlite3* db = nullptr;
    DIR* stream = nullptr;
    std::string path, stamp, error;
    sqlite3_int64 folder = -1, epoch = -1, count = -1, scanned = 0;
    bool persistent = false;
    Statement insert{nullptr, sqlite3_finalize};

    ~SlateDirectory() {
        insert.reset();
        if (db) sqlite3_close(db);
        if (stream) closedir(stream);
    }
    bool fail(const char* message) {
        error = message;
        return false;
    }
    bool start(const char* location, const char* database) {
        path = location;
        // Check actual directory access even on a cache hit.
        const int fd = open(location, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) return fail(strerror(errno));
        stream = fdopendir(fd);
        if (!stream) { const auto saved = errno; close(fd); return fail(strerror(saved)); }
        struct stat status{};
        if (fstat(fd, &status)) return fail(strerror(errno));
        stamp = version(status);
        if (database && *database && sqlite3_open(database, &db) == SQLITE_OK) {
            sqlite3_busy_timeout(db, 1500);
            persistent = execute(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;") && execute(db, schema);
        }
        if (!persistent) {
            if (db) sqlite3_close(db);
            // Empty SQLite filename uses a private temporary on-disk database.
            db = nullptr;
            if (sqlite3_open("", &db) != SQLITE_OK) return fail("Could not allocate a directory listing.");
        }
        execute(db, "PRAGMA temp_store=FILE; PRAGMA cache_size=-256; PRAGMA temp.cache_size=-256;");
        if (persistent) {
            auto snapshot = prepare(db, "SELECT id,n FROM folders WHERE path=? AND version=?");
            if (snapshot) {
                bind(snapshot.get(), 1, path);
                bind(snapshot.get(), 2, stamp);
                if (sqlite3_step(snapshot.get()) == SQLITE_ROW) {
                    folder = sqlite3_column_int64(snapshot.get(), 0);
                    count = sqlite3_column_int64(snapshot.get(), 1);
                    return true;
                }
            }
            auto revision = prepare(db, "SELECT epoch FROM invalidation WHERE id=1");
            if (revision && sqlite3_step(revision.get()) == SQLITE_ROW) epoch = sqlite3_column_int64(revision.get(), 0);
        }
        if (!execute(db, "CREATE TEMP TABLE listing(ordinal INTEGER PRIMARY KEY,name TEXT,dir INTEGER)")) return fail(sqlite3_errmsg(db));
        insert = prepare(db, "INSERT INTO temp.listing VALUES(?,?,?)");
        return insert || fail(sqlite3_errmsg(db));
    }

    // Only complete, unchanged large listings become shared snapshots. Private
    // staging tables keep concurrent searches and interrupted scans isolated.
    void publish() {
        if (!persistent || count < 128 || epoch < 0) return;
        struct stat current{};
        if (fstat(dirfd(stream), &current) || version(current) != stamp || !execute(db, "BEGIN IMMEDIATE")) return;
        auto replace = prepare(db, "INSERT INTO folders(path,version,n) SELECT ?,?,? WHERE "
                               "(SELECT epoch FROM invalidation WHERE id=1)=? "
                               "ON CONFLICT(path) DO UPDATE SET version=excluded.version,n=excluded.n");
        bool ok = false;
        if (replace) {
            bind(replace.get(), 1, path);
            bind(replace.get(), 2, stamp);
            sqlite3_bind_int64(replace.get(), 3, count);
            sqlite3_bind_int64(replace.get(), 4, epoch);
            ok = sqlite3_step(replace.get()) == SQLITE_DONE && sqlite3_changes(db) == 1;
        }
        auto remove = prepare(db, "DELETE FROM names WHERE folder=(SELECT id FROM folders WHERE path=?)");
        auto copy = prepare(db, "INSERT INTO names SELECT (SELECT id FROM folders WHERE path=?),ordinal,name,dir FROM temp.listing");
        if (ok && remove && copy) {
            bind(remove.get(), 1, path);
            bind(copy.get(), 1, path);
            ok = sqlite3_step(remove.get()) == SQLITE_DONE && sqlite3_step(copy.get()) == SQLITE_DONE;
        } else { ok = false; }
        if (!ok || !execute(db, "COMMIT")) execute(db, "ROLLBACK");
    }

    bool warm(int budget) {
        if (!error.empty() || count >= 0) return error.empty();
        while (budget-- > 0) {
            errno = 0;
            auto* entry = readdir(stream);
            if (!entry) {
                if (errno == EINTR) { ++budget; continue; }
                if (errno) return fail(strerror(errno));
                count = scanned;
                publish();
                return true;
            }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            auto type = entry->d_type;
            if (type == DT_UNKNOWN) {
                struct stat info{};
                if (fstatat(dirfd(stream), entry->d_name, &info, AT_SYMLINK_NOFOLLOW)) continue;
                type = S_ISDIR(info.st_mode) ? DT_DIR : S_ISREG(info.st_mode) ? DT_REG : DT_UNKNOWN;
            }
            if (type != DT_DIR && type != DT_REG) continue;
            sqlite3_reset(insert.get());
            sqlite3_bind_int64(insert.get(), 1, scanned);
            sqlite3_bind_text(insert.get(), 2, entry->d_name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(insert.get(), 3, type == DT_DIR);
            if (sqlite3_step(insert.get()) != SQLITE_DONE) return fail(sqlite3_errmsg(db));
            ++scanned;
        }
        return true;
    }

    int page(sqlite3_int64 offset, int limit, void* context, SlateDirectoryRow row,
             const char* prefix = nullptr, const char* needle = nullptr) {
        if (!error.empty() || offset < 0 || limit <= 0) return -1;
        while (count < 0 && scanned < offset + limit) {
            if (!warm(static_cast<int>(std::min<sqlite3_int64>(512, offset + limit - scanned)))) return -1;
        }
        std::string sql = folder < 0 ? "SELECT name,dir FROM temp.listing WHERE ordinal>=?1" :
            "SELECT name,dir FROM names WHERE ordinal>=?1 AND folder=?3 AND "
            "EXISTS(SELECT 1 FROM folders WHERE id=?3 AND version=?4 AND path=?5)";
        sql += " AND ordinal<?2";
        if (needle) sql += " AND (dir=1 OR instr(lower(?6||name),lower(?7))>0)";
        sql += " ORDER BY ordinal";
        auto query = prepare(db, sql.c_str());
        if (!query) { fail(sqlite3_errmsg(db)); return -1; }
        sqlite3_bind_int64(query.get(), 1, offset);
        sqlite3_bind_int64(query.get(), 2, offset + limit);
        if (folder >= 0) {
            sqlite3_bind_int64(query.get(), 3, folder);
            bind(query.get(), 4, stamp);
            bind(query.get(), 5, path);
        }
        if (needle) {
            sqlite3_bind_text(query.get(), 6, prefix, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(query.get(), 7, needle, -1, SQLITE_TRANSIENT);
        }
        int n = 0, step;
        while ((step = sqlite3_step(query.get())) == SQLITE_ROW) {
            row(context, reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 0)), sqlite3_column_int(query.get(), 1));
            ++n;
        }
        if (step != SQLITE_DONE) { fail(sqlite3_errmsg(db)); return -1; }
        if (!needle && folder >= 0 && n < std::min<sqlite3_int64>(limit, std::max<sqlite3_int64>(0, count - offset))) {
            fail("The folder changed. Refresh its listing."); return -1;
        }
        if (needle) return static_cast<int>(std::clamp<sqlite3_int64>((count < 0 ? scanned : count) - offset, 0, limit));
        return n;
    }
};

SlateDirectory* slate_directory_open(const char* path, const char* database) {
    auto listing = std::make_unique<SlateDirectory>();
    listing->start(path, database);
    return listing.release();
}
void slate_directory_close(SlateDirectory* dir) { delete dir; }
int slate_directory_page(SlateDirectory* dir, int64_t offset, int limit, void* ctx, SlateDirectoryRow row) {
    return dir->page(offset, limit, ctx, row);
}
int slate_directory_matches(SlateDirectory* dir, int64_t offset, int limit, const char* prefix,
                           const char* query, void* ctx, SlateDirectoryRow row) {
    return dir->page(offset, limit, ctx, row, prefix, query);
}
int slate_directory_warm(SlateDirectory* dir, int budget) { return dir->warm(budget) ? 1 : 0; }
int64_t slate_directory_count(const SlateDirectory* dir) { return dir->count; }
int64_t slate_directory_scanned(const SlateDirectory* dir) { return dir->scanned; }
int slate_directory_cached(const SlateDirectory* dir) { return dir->folder >= 0; }
const char* slate_directory_error(const SlateDirectory* dir) { return dir->error.c_str(); }

void slate_directory_forget(const char* database, const char* root) {
    if (!database || !*database) return;
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(database, &raw, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        sqlite3_close(raw); return;
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    sqlite3_busy_timeout(raw, 1500);
    if (!execute(raw, "BEGIN IMMEDIATE")) return;
    const std::string path(root);
    auto select = "SELECT id FROM folders WHERE path=? OR substr(path,1,length(?)+1)=?||'/'";
    auto purge = [&](const std::string& sql) {
        auto stmt = prepare(raw, sql.c_str());
        if (!stmt) return false;
        for (int i = 1; i <= 3; ++i) bind(stmt.get(), i, path);
        return sqlite3_step(stmt.get()) == SQLITE_DONE;
    };
    if (purge(std::string("DELETE FROM names WHERE folder IN (") + select + ")") &&
        purge(std::string("DELETE FROM folders WHERE id IN (") + select + ")") &&
        execute(raw, "UPDATE invalidation SET epoch=epoch+1; COMMIT")) return;
    execute(raw, "ROLLBACK");
}
