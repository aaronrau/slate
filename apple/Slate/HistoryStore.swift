import Foundation
import SQLite3

// Indexed reads fetch a strip of labels or a single bookmark. Visiting a file writes
// one integer pair, independently of the number of past visits and distinct files.
final class HistoryStore {
    private var database: OpaquePointer?
    private let transient = unsafeBitCast(-1, to: sqlite3_destructor_type.self)

    static func applicationStore() -> HistoryStore? {
        let fm = FileManager.default
        guard let directory = try? fm.url(for: .applicationSupportDirectory, in: .userDomainMask,
                                          appropriateFor: nil, create: true).appendingPathComponent("Slate", isDirectory: true),
              (try? fm.createDirectory(at: directory, withIntermediateDirectories: true)) != nil else { return nil }
        return HistoryStore(url: directory.appendingPathComponent("history.sqlite"))
    }

    static func inMemory() -> HistoryStore {
        guard let store = HistoryStore(path: ":memory:") else { fatalError("Unable to allocate navigation history") }
        return store
    }
    convenience init?(url: URL) { self.init(path: url.path) }

    private init?(path: String) {
        guard sqlite3_open(path, &database) == SQLITE_OK else { return nil }
        guard run("""
            PRAGMA journal_mode=WAL;
            PRAGMA synchronous=NORMAL;
            CREATE TABLE IF NOT EXISTS tabs(id INTEGER PRIMARY KEY, file BLOB NOT NULL, caret INTEGER DEFAULT 0,
                                            preview INTEGER DEFAULT 0, path TEXT);
            CREATE TABLE IF NOT EXISTS visits(position INTEGER PRIMARY KEY, tab INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS state(id INTEGER PRIMARY KEY, position INTEGER NOT NULL, showing INTEGER NOT NULL);
            CREATE TEMP TABLE unavailable(id INTEGER PRIMARY KEY);
            """) else { return nil }
        guard migrateTabPaths(), run("CREATE INDEX IF NOT EXISTS tab_paths ON tabs(path)") else { return nil }
    }
    deinit { sqlite3_close(database) }

    @discardableResult
    private func run(_ sql: String) -> Bool { sqlite3_exec(database, sql, nil, nil, nil) == SQLITE_OK }

    private func statement<T>(_ sql: String, _ body: (OpaquePointer) -> T?) -> T? {
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(database, sql, -1, &statement, nil) == SQLITE_OK, let statement else { return nil }
        defer { sqlite3_finalize(statement) }
        return body(statement)
    }

    private func value(_ sql: String) -> Int? {
        statement(sql) { stmt in
            guard sqlite3_step(stmt) == SQLITE_ROW, sqlite3_column_type(stmt, 0) != SQLITE_NULL else { return nil }
            return Int(sqlite3_column_int64(stmt, 0))
        }
    }

    private func bind(_ text: String, to stmt: OpaquePointer, at index: Int32) {
        sqlite3_bind_text(stmt, index, text, -1, transient)
    }

    private func reference(_ stmt: OpaquePointer, column: Int32) -> FileReference? {
        guard let blob = sqlite3_column_blob(stmt, column) else { return nil }
        let data = Data(bytes: blob, count: Int(sqlite3_column_bytes(stmt, column)))
        return try? JSONDecoder().decode(FileReference.self, from: data)
    }

    // The original schema kept the URL inside the encoded bookmark. Backfill the
    // indexed label one row at a time, once, without retaining decoded bookmarks.
    private func migrateTabPaths() -> Bool {
        if value("SELECT count(*) FROM pragma_table_info('tabs') WHERE name='path'") == 1 { return true }
        guard run("BEGIN IMMEDIATE"), run("ALTER TABLE tabs ADD COLUMN path TEXT") else { return false }
        let complete = statement("SELECT id,file FROM tabs ORDER BY id") { rows -> Bool? in
            var result = sqlite3_step(rows)
            while result == SQLITE_ROW {
                let id = sqlite3_column_int64(rows, 0)
                let path = autoreleasepool { reference(rows, column: 1)?.url.standardizedFileURL.path }
                if let path {
                    let updated = statement("UPDATE tabs SET path=? WHERE id=?") { stmt -> Bool? in
                        bind(path, to: stmt, at: 1)
                        sqlite3_bind_int64(stmt, 2, id)
                        return sqlite3_step(stmt) == SQLITE_DONE
                    }
                    if updated != true { return false }
                }
                result = sqlite3_step(rows)
            }
            return result == SQLITE_DONE
        }
        guard complete == true, run("COMMIT") else { run("ROLLBACK"); return false }
        return true
    }

    var tabCount: Int { (value("SELECT MAX(id) FROM tabs") ?? -1) + 1 }
    var position: Int { value("SELECT position FROM state WHERE id=1") ?? -1 }
    var showing: Bool { value("SELECT showing FROM state WHERE id=1") == 1 }

    func tabID(at visit: Int) -> Int? { value("SELECT tab FROM visits WHERE position=\(visit)") }

    func visit(starting position: Int, backward: Bool) -> Int? {
        // Temporarily skip inaccessible files in both directions; clicking the tab
        // after a provider reconnects retries its bookmark and restores its visits.
        let comparison = backward ? "<=" : ">="
        let order = backward ? "DESC" : "ASC"
        return value("""
            SELECT position FROM visits WHERE position \(comparison) \(position)
            AND NOT EXISTS (SELECT 1 FROM unavailable WHERE id=visits.tab)
            ORDER BY position \(order) LIMIT 1
            """)
    }

    func markUnavailable(_ id: Int) { run("INSERT OR IGNORE INTO unavailable VALUES(\(id))") }

    func tabID(path: String) -> Int? {
        statement("SELECT id FROM tabs WHERE path=? ORDER BY id LIMIT 1") { stmt in
            bind(path, to: stmt, at: 1)
            guard sqlite3_step(stmt) == SQLITE_ROW else { return nil }
            return Int(sqlite3_column_int64(stmt, 0))
        }
    }

    func tabs(from start: Int, limit: Int) -> [NavigationHistory.Tab] {
        statement("SELECT id,path,caret,preview FROM tabs WHERE id>=? ORDER BY id LIMIT ?") { stmt in
            sqlite3_bind_int64(stmt, 1, Int64(start))
            sqlite3_bind_int64(stmt, 2, Int64(limit))
            var tabs: [NavigationHistory.Tab] = []
            while sqlite3_step(stmt) == SQLITE_ROW {
                let id = Int(sqlite3_column_int64(stmt, 0))
                let path = sqlite3_column_text(stmt, 1).map { String(cString: $0) } ?? "Unavailable file"
                tabs.append(.init(id: id, path: path, caret: UInt64(max(0, sqlite3_column_int64(stmt, 2))),
                                  preview: sqlite3_column_int(stmt, 3) != 0))
            }
            return tabs
        } ?? []
    }

    func file(for id: Int) -> FileReference? {
        statement("SELECT file FROM tabs WHERE id=?") { stmt in
            sqlite3_bind_int64(stmt, 1, Int64(id))
            guard sqlite3_step(stmt) == SQLITE_ROW else { return nil }
            return reference(stmt, column: 0)
        }
    }

    func record(file: ScopedURL, id: Int, position: Int, appended: Bool) throws {
        let data = try JSONEncoder().encode(file.reference)
        guard run("BEGIN IMMEDIATE") else { throw failure }
        let saved = statement("""
            INSERT INTO tabs(id,file,path) VALUES(?,?,?)
            ON CONFLICT(id) DO UPDATE SET file=excluded.file,path=excluded.path
            """) { stmt -> Bool? in
                sqlite3_bind_int64(stmt, 1, Int64(id))
                _ = data.withUnsafeBytes { sqlite3_bind_blob(stmt, 2, $0.baseAddress, Int32($0.count), transient) }
                bind(file.url.standardizedFileURL.path, to: stmt, at: 3)
                return sqlite3_step(stmt) == SQLITE_DONE
        }
        let appendedVisit = !appended || (run("DELETE FROM visits WHERE position >= \(position)") &&
                                          run("INSERT INTO visits VALUES (\(position), \(id))"))
        guard saved == true, appendedVisit, run("DELETE FROM unavailable WHERE id=\(id)"),
              setPosition(position, showing: true), run("COMMIT") else {
            let error = failure
            run("ROLLBACK")
            throw error
        }
    }

    @discardableResult
    func setPosition(_ position: Int, showing: Bool) -> Bool {
        run("INSERT OR REPLACE INTO state VALUES(1, \(position), \(showing ? 1 : 0))")
    }

    func remember(_ tabID: Int, caret: UInt64, preview: Bool) {
        run("UPDATE tabs SET caret=\(caret), preview=\(preview ? 1 : 0) WHERE id=\(tabID)")
    }

    private var failure: EditorFailure { EditorFailure("Navigation history: " + String(cString: sqlite3_errmsg(database))) }
}
