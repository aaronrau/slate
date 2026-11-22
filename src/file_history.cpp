#include "file_history.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

namespace slate {
namespace {

void check(sqlite3* db, int result) {
    if (result != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
}

void exec(sqlite3* db, const char* sql) {
    check(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}

class Statement {
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        check(db, sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr));
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    void number(int column, std::int64_t n) {
        check(db_, sqlite3_bind_int64(statement_, column, n));
    }
    void path(int column, const std::filesystem::path& p) {
        const auto& bytes = p.native();
        check(db_, sqlite3_bind_blob64(statement_, column, bytes.data(), bytes.size(), SQLITE_TRANSIENT));
    }
    bool step() {
        int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) return true;
        if (result != SQLITE_DONE) check(db_, result);
        return false;
    }
    std::int64_t integer(int column) const { return sqlite3_column_int64(statement_, column); }
    HistoryFile file() const {
        const auto* bytes = static_cast<const char*>(sqlite3_column_blob(statement_, 0));
        auto preview = sqlite3_column_type(statement_, 2) == SQLITE_NULL ?
            std::nullopt : std::optional<bool>(integer(2) != 0);
        return {std::string(bytes ? bytes : "", sqlite3_column_bytes(statement_, 0)),
                static_cast<std::uint64_t>(integer(1)), preview, integer(3)};
    }
    std::optional<HistoryFile> one() { return step() ? std::optional(file()) : std::nullopt; }
};

// The cursor identifies a visit. Relative lookups use the INTEGER PRIMARY KEY
// index in both directions, even after a long session and after restarting.
constexpr auto current_sql =
    "SELECT f.path,f.cursor,f.preview,v.id FROM state s JOIN visits v ON v.id=s.here "
    "JOIN files f ON f.id=v.file";
constexpr auto previous_sql =
    "SELECT f.path,f.cursor,f.preview,v.id FROM visits v JOIN files f ON f.id=v.file "
    "WHERE v.id<coalesce(nullif(?,0),(SELECT here FROM state)) ORDER BY v.id DESC LIMIT 1";
constexpr auto next_sql =
    "SELECT f.path,f.cursor,f.preview,v.id FROM visits v JOIN files f ON f.id=v.file "
    "WHERE v.id>coalesce(nullif(?,0),(SELECT here FROM state)) ORDER BY v.id LIMIT 1";

} // namespace

FileHistory::FileHistory(const std::filesystem::path& database) {
    try {
        initialize(database);
    } catch (const std::exception& e) {
        sqlite3_close(db_);
        db_ = nullptr;
        if (database.empty()) throw;
        // A second window on this workspace gets its own navigation cursor.
        std::cerr << "Using temporary navigation history: " << e.what() << '\n';
        try { initialize({}); }
        catch (...) { sqlite3_close(db_); throw; }
    }
}

void FileHistory::initialize(const std::filesystem::path& path) {
    const int opened = sqlite3_open_v2(path.c_str(), &db_,
                                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    check(db_, opened);
    exec(db_, "PRAGMA locking_mode=EXCLUSIVE; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
              "PRAGMA cache_size=-512; BEGIN EXCLUSIVE;"
              "CREATE TABLE IF NOT EXISTS files(id INTEGER PRIMARY KEY,path BLOB UNIQUE NOT NULL,"
                "cursor INTEGER NOT NULL DEFAULT 0,preview INTEGER);"
              "CREATE TABLE IF NOT EXISTS tabs(id INTEGER PRIMARY KEY,file INTEGER UNIQUE NOT NULL);"
              "CREATE TABLE IF NOT EXISTS visits(id INTEGER PRIMARY KEY,file INTEGER NOT NULL);"
              "CREATE TABLE IF NOT EXISTS state(id INTEGER PRIMARY KEY,here INTEGER NOT NULL);"
              "INSERT OR IGNORE INTO state VALUES(1,0); COMMIT;");
}

FileHistory::~FileHistory() { sqlite3_close(db_); }

std::optional<HistoryFile> FileHistory::current() const { return Statement(db_, current_sql).one(); }
std::optional<HistoryFile> FileHistory::previous(std::int64_t from) const {
    Statement q(db_, previous_sql);
    q.number(1, from);
    return q.one();
}
std::optional<HistoryFile> FileHistory::next(std::int64_t from) const {
    Statement q(db_, next_sql);
    q.number(1, from);
    return q.one();
}
std::optional<HistoryFile> FileHistory::position(const std::filesystem::path& path) const {
    Statement q(db_, "SELECT path,cursor,preview,0 FROM files WHERE path=?");
    q.path(1, path);
    return q.one();
}

std::vector<HistoryFile> FileHistory::nearby_tabs() const {
    Statement center(db_, "SELECT t.id FROM state s JOIN visits v ON v.id=s.here "
                          "JOIN tabs t ON t.file=v.file");
    if (!center.step()) return {};
    auto tab = center.integer(0);
    std::vector<HistoryFile> result;
    Statement left(db_, "SELECT f.path,f.cursor,f.preview,0 FROM tabs t JOIN files f ON f.id=t.file "
                        "WHERE t.id<=? ORDER BY t.id DESC LIMIT 3");
    left.number(1, tab);
    while (left.step()) result.push_back(left.file());
    std::reverse(result.begin(), result.end());
    Statement right(db_, "SELECT f.path,f.cursor,f.preview,0 FROM tabs t JOIN files f ON f.id=t.file "
                         "WHERE t.id>? ORDER BY t.id LIMIT 2");
    right.number(1, tab);
    while (right.step()) result.push_back(right.file());
    return result;
}

void FileHistory::remember(const std::filesystem::path& path, std::uint64_t cursor, bool preview) {
    Statement q(db_, "UPDATE files SET cursor=?,preview=? WHERE path=? "
                     "AND (cursor<>?1 OR preview IS NOT ?2)");
    q.number(1, cursor);
    q.number(2, preview);
    q.path(3, path);
    q.step();
}

void FileHistory::visit(const std::filesystem::path& path, std::int64_t existing_visit,
                        bool close_current_tab) {
    change(path, existing_visit, false, close_current_tab);
}

void FileHistory::saved_as(const std::filesystem::path& path) {
    change(path, 0, true, true);
}

void FileHistory::change(const std::filesystem::path& path, std::int64_t existing_visit,
                         bool replace_visit, bool close_current_tab) {
    exec(db_, "BEGIN");
    try {
        Statement create(db_, "INSERT OR IGNORE INTO files(path) VALUES(?)");
        create.path(1, path);
        create.step();
        Statement lookup(db_, "SELECT id FROM files WHERE path=?");
        lookup.path(1, path);
        lookup.step();
        auto target = lookup.integer(0);
        Statement current(db_, "SELECT s.here,v.file FROM state s LEFT JOIN visits v ON v.id=s.here");
        current.step();
        auto here = current.integer(0), old_file = current.integer(1);
        if (close_current_tab && old_file != target) {
            Statement close(db_, "DELETE FROM tabs WHERE file=?");
            close.number(1, old_file);
            close.step();
        }
        Statement tab(db_, "INSERT OR IGNORE INTO tabs(file) VALUES(?)");
        tab.number(1, target);
        tab.step();
        if (existing_visit) {
            here = existing_visit;
        } else if (replace_visit && here) {
            Statement rename(db_, "UPDATE visits SET file=? WHERE id=?");
            rename.number(1, target);
            rename.number(2, here);
            rename.step();
        } else if (old_file != target) {
            Statement branch(db_, "DELETE FROM visits WHERE id>?");
            branch.number(1, here);
            branch.step();
            Statement add(db_, "INSERT INTO visits(file) VALUES(?)");
            add.number(1, target);
            add.step();
            here = sqlite3_last_insert_rowid(db_);
        }
        Statement move(db_, "UPDATE state SET here=? WHERE id=1");
        move.number(1, here);
        move.step();
        exec(db_, "COMMIT");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

} // namespace slate
