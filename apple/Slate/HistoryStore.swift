import Foundation
import SQLite3

// Each click writes one visit ID. Bookmark data is stored once per tab.
// Pruning a forward branch never discards a tab.
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

    init?(url: URL) {
        guard sqlite3_open(url.path, &database) == SQLITE_OK else { return nil }
        guard run("""
            PRAGMA journal_mode=WAL;
            PRAGMA synchronous=NORMAL;
            CREATE TABLE IF NOT EXISTS tabs(id INTEGER PRIMARY KEY, file BLOB NOT NULL, caret INTEGER DEFAULT 0, preview INTEGER DEFAULT 0);
            CREATE TABLE IF NOT EXISTS visits(position INTEGER PRIMARY KEY, tab INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS state(id INTEGER PRIMARY KEY, position INTEGER NOT NULL, showing INTEGER NOT NULL);
            """) else { return nil }
    }
    deinit { sqlite3_close(database) }

    @discardableResult
    private func run(_ sql: String) -> Bool { sqlite3_exec(database, sql, nil, nil, nil) == SQLITE_OK }

    private func statement(_ sql: String, _ body: (OpaquePointer) -> Void) {
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(database, sql, -1, &statement, nil) == SQLITE_OK, let statement else { return }
        defer { sqlite3_finalize(statement) }
        body(statement)
    }

    func load() -> ([NavigationHistory.Tab], [Int], Int, Bool) {
        var tabs: [NavigationHistory.Tab] = [], visits: [Int] = []
        var position = -1, showing = false
        statement("SELECT id,file,caret,preview FROM tabs ORDER BY id") { stmt in
            while sqlite3_step(stmt) == SQLITE_ROW {
                let id = Int(sqlite3_column_int64(stmt, 0))
                guard id == tabs.count, let blob = sqlite3_column_blob(stmt, 1) else { break }
                let data = Data(bytes: blob, count: Int(sqlite3_column_bytes(stmt, 1)))
                guard let file = try? JSONDecoder().decode(FileReference.self, from: data) else { break }
                tabs.append(.init(id: id, reference: file, caret: UInt64(max(0, sqlite3_column_int64(stmt, 2))),
                                  preview: sqlite3_column_int(stmt, 3) != 0))
            }
        }
        statement("SELECT tab FROM visits ORDER BY position") { stmt in
            while sqlite3_step(stmt) == SQLITE_ROW {
                let tab = Int(sqlite3_column_int64(stmt, 0))
                guard tabs.indices.contains(tab) else { break }
                visits.append(tab)
            }
        }
        statement("SELECT position,showing FROM state WHERE id=1") { stmt in
            if sqlite3_step(stmt) == SQLITE_ROW {
                position = Int(sqlite3_column_int64(stmt, 0))
                showing = sqlite3_column_int(stmt, 1) != 0
            }
        }
        position = min(max(0, position), visits.count - 1)
        return (tabs, visits, position, showing && position >= 0)
    }

    func record(tab: NavigationHistory.Tab, position: Int, appended: Bool) {
        run("BEGIN IMMEDIATE")
        if let data = try? JSONEncoder().encode(tab.reference) {
            statement("INSERT INTO tabs(id,file,caret,preview) VALUES(?,?,?,?) ON CONFLICT(id) DO UPDATE SET file=excluded.file") { stmt in
                sqlite3_bind_int64(stmt, 1, Int64(tab.id))
                _ = data.withUnsafeBytes { sqlite3_bind_blob(stmt, 2, $0.baseAddress, Int32($0.count), transient) }
                sqlite3_bind_int64(stmt, 3, Int64(clamping: tab.caret))
                sqlite3_bind_int(stmt, 4, tab.preview ? 1 : 0)
                sqlite3_step(stmt)
            }
        }
        if appended {
            run("DELETE FROM visits WHERE position >= \(position)")
            run("INSERT INTO visits VALUES (\(position), \(tab.id))")
        }
        setPosition(position, showing: true)
        run("COMMIT")
    }

    func setPosition(_ position: Int, showing: Bool) {
        run("INSERT OR REPLACE INTO state VALUES(1, \(position), \(showing ? 1 : 0))")
    }

    func remember(_ tab: NavigationHistory.Tab) {
        run("UPDATE tabs SET caret=\(tab.caret), preview=\(tab.preview ? 1 : 0) WHERE id=\(tab.id)")
    }
}
