import XCTest
import WebKit
import SQLite3
@testable import Slate

final class SlateTests: XCTestCase {
    func testAdjacentUTF8PagesAndUnsavedPreview() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let file = dir.appendingPathComponent("pages.md")
        let line = String(repeating: "x", count: Document.pageSize - 1)
        try (line + "🌱 end").write(to: file, atomically: true, encoding: .utf8)
        let doc = Document()
        try doc.open(file)
        var first = try doc.page(starting: 0)
        let second = try doc.page(starting: first.end)
        XCTAssertTrue(first.isUTF8 && second.isUTF8)
        XCTAssertEqual(first.text + second.text, line + "🌱 end")
        try first.update("# " + first.text, in: doc)
        XCTAssertTrue(doc.dirty)
        try doc.save(file)
        XCTAssertEqual(try String(contentsOf: file, encoding: .utf8), "# " + line + "🌱 end")
    }

    func testChangingMalformedPageKeepsOriginalBytes() throws {
        let file = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: file) }
        try Data([0x61, 0xff, 0x62]).write(to: file)
        let doc = Document()
        try doc.open(file)
        var page = try doc.page(starting: 0)
        XCTAssertFalse(page.isUTF8)
        XCTAssertThrowsError(try page.update("a�c", in: doc))
        XCTAssertFalse(doc.dirty)
        XCTAssertEqual(try doc.read(from: 0, length: 3), [0x61, 0xff, 0x62])
    }

    func testHistoryKeepsDistinctTabsAndRestoresBranchAndPosition() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let database = dir.appendingPathComponent("test.sqlite")
        let store = try XCTUnwrap(HistoryStore(url: database))
        let history = NavigationHistory(store: store)
        let files = try ["a", "b", "c"].map { name -> ScopedURL in
            let url = dir.appendingPathComponent(name)
            try Data().write(to: url)
            return ScopedURL(url)
        }
        for index in [0, 1, 0, 1] { try history.record(files[index]) }
        let prior = try XCTUnwrap(history.back)
        try history.record(files[0], at: prior)
        XCTAssertEqual(history.tab(atVisit: try XCTUnwrap(history.forward))?.name, "b")
        try history.record(files[2])
        history.remember(caret: 4099, preview: true)
        XCTAssertNil(history.forward)
        let restored = NavigationHistory(store: try XCTUnwrap(HistoryStore(url: database)))
        XCTAssertEqual(restored.visibleTabs.map(\.name), ["a", "b", "c"])
        let current = try XCTUnwrap(restored.active)
        XCTAssertEqual(restored.tab(id: current)?.caret, 4099)
        XCTAssertEqual(restored.tab(id: current)?.preview, true)
        XCTAssertEqual(restored.tab(atVisit: try XCTUnwrap(restored.back))?.name, "a")
    }

    func testLargeHistoryRestoresOnlyTheVisibleLabelsAndMigratesOldBookmarks() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let database = dir.appendingPathComponent("legacy.sqlite")
        var raw: OpaquePointer?
        XCTAssertEqual(sqlite3_open(database.path, &raw), SQLITE_OK)
        let seed = """
            CREATE TABLE tabs(id INTEGER PRIMARY KEY, file BLOB NOT NULL, caret INTEGER DEFAULT 0, preview INTEGER DEFAULT 0);
            CREATE TABLE visits(position INTEGER PRIMARY KEY, tab INTEGER NOT NULL);
            CREATE TABLE state(id INTEGER PRIMARY KEY, position INTEGER NOT NULL, showing INTEGER NOT NULL);
            WITH RECURSIVE n(i) AS (VALUES(0) UNION ALL SELECT i+1 FROM n WHERE i<10239)
            INSERT INTO tabs(id,file,caret) SELECT i,CAST('{"url":"file:///tmp/note-' || i || '.txt"}' AS BLOB),i FROM n;
            INSERT INTO visits SELECT id,id FROM tabs;
            INSERT INTO state VALUES(1,5021,1);
            """
        XCTAssertEqual(sqlite3_exec(raw, seed, nil, nil, nil), SQLITE_OK)
        sqlite3_close(raw)

        let store = try XCTUnwrap(HistoryStore(url: database))
        // A damaged, unrelated bookmark must never be decoded to construct the strip.
        XCTAssertEqual(sqlite3_open(database.path, &raw), SQLITE_OK)
        XCTAssertEqual(sqlite3_exec(raw, "UPDATE tabs SET file=x'ff' WHERE id=5020", nil, nil, nil), SQLITE_OK)
        sqlite3_close(raw)
        let history = NavigationHistory(store: store)
        XCTAssertEqual(history.tabCount, 10240)
        XCTAssertEqual(history.visibleTabs.count, NavigationHistory.stripSize)
        XCTAssertEqual(history.active, 5021)
        let saved = try XCTUnwrap(history.tab(atVisit: 5021))
        XCTAssertEqual(try history.acquire(saved).url.lastPathComponent, "note-5021.txt")
        XCTAssertEqual(saved.caret, 5021)
        let damaged = try XCTUnwrap(history.tab(atVisit: 5020))
        XCTAssertEqual(damaged.name, "note-5020.txt")
        XCTAssertThrowsError(try history.acquire(damaged))
        history.markUnavailable(damaged.id)
        XCTAssertEqual(history.back, 5019)
        history.shiftStrip(forward: true)
        XCTAssertEqual(history.visibleTabs.first?.id, 5024)
        XCTAssertTrue(history.hasEarlierTabs && history.hasLaterTabs)
        history.shiftStrip(forward: false)
        XCTAssertTrue(history.visibleTabs.contains { $0.id == 5021 })
    }

    @MainActor
    private func waitUntil(_ condition: () -> Bool, file: StaticString = #filePath, line: UInt = #line) async throws {
        for _ in 0..<250 {
            if condition() { return }
            try await Task.sleep(nanoseconds: 20_000_000)
        }
        XCTFail("Timed out waiting for editor operation", file: file, line: line)
    }

    @MainActor
    func testEditingSavingOpeningAndSearchingInWorkspace() async throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let first = dir.appendingPathComponent("notes.md")
        let second = dir.appendingPathComponent("log.txt")
        try Data("# Notes".utf8).write(to: first)
        try Data("Second file".utf8).write(to: second)

        let model = EditorModel(history: NavigationHistory())
        model.open(first)
        try await waitUntil { !model.busy }
        XCTAssertTrue(model.preview)
        model.preview = false
        let replacement = "# Notes\n🍏 needle"
        model.edit(replacement, caret: NSRange(location: 0, length: 0), composing: false)
        model.open(second)
        XCTAssertTrue(model.confirmDiscard)
        model.cancelDiscard()
        XCTAssertEqual(model.file?.url, first)
        XCTAssertTrue(model.dirty)
        model.save()
        try await waitUntil { !model.busy }
        XCTAssertNil(model.errorMessage)
        XCTAssertFalse(model.dirty)
        XCTAssertEqual(try String(contentsOf: first, encoding: .utf8), replacement)

        let workspace = WorkspaceModel()
        workspace.open(folder: dir)
        let branch = try XCTUnwrap(workspace.expanded.values.first)
        try await waitUntil { !branch.busy }
        XCTAssertEqual(Set(branch.rows.map { $0.url.lastPathComponent }), ["notes.md", "log.txt"])
        workspace.contents = true
        workspace.query = "needle"
        try await waitUntil { !workspace.searching }
        let hit = try XCTUnwrap(workspace.hits.first)
        XCTAssertEqual(hit.offset, UInt64("# Notes\n🍏 ".utf8.count))
        model.open(second)
        model.openExternal(first)
        try await waitUntil { !model.busy }
        model.resumePendingOpen()
        try await waitUntil { !model.busy }
        XCTAssertEqual(model.file?.url, first)
        model.open(hit.url, workspace: try workspace.access(for: hit), offset: hit.offset, length: hit.matchLength)
        XCTAssertFalse(model.preview)
        XCTAssertEqual((model.page.text as NSString).substring(with: model.selection), "needle")
        let visit = try XCTUnwrap(model.history.back)
        model.navigate(visit: visit)
        try await waitUntil { !model.busy }
        XCTAssertEqual(model.file?.url.standardizedFileURL, second.standardizedFileURL)
    }

    @MainActor
    func testFilesTreeRetainsGrantedLocationsAndReleasesCollapsedPages() async throws {
        let name = UUID().uuidString
        let defaults = try XCTUnwrap(UserDefaults(suiteName: name))
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent(name)
        defer { defaults.removePersistentDomain(forName: name); try? fm.removeItem(at: root) }
        let local = root.appendingPathComponent("Local")
        let first = root.appendingPathComponent("First")
        let folder = first.appendingPathComponent("Tree")
        let second = root.appendingPathComponent("Second")
        for url in [local, folder, second] { try fm.createDirectory(at: url, withIntermediateDirectories: true) }
        for n in 0..<150 { try Data().write(to: folder.appendingPathComponent("note-\(n).txt")) }

        var original: WorkspaceModel? = WorkspaceModel(defaults: defaults, documents: local)
        original?.open(folder: first)
        original?.open(folder: second)
        original = nil
        let restored = WorkspaceModel(defaults: defaults, documents: local)
        XCTAssertEqual(restored.locations.map(\.title), ["Slate Documents", "First", "Second"])
        let location = try XCTUnwrap(restored.locations.first(where: { $0.title == "First" }))
        XCTAssertNil(restored.expanded[location.id])
        restored.toggle(location)
        let branch = try XCTUnwrap(restored.expanded[location.id])
        try await waitUntil { !branch.busy }
        let child = try XCTUnwrap(branch.rows.first?.url)
        branch.toggle(child)
        var files = try XCTUnwrap(branch.children[child])
        try await waitUntil { !files.busy }
        XCTAssertEqual(files.rows.count, 128)
        XCTAssertTrue(files.more)
        files.loadMore()
        try await waitUntil { !files.busy }
        XCTAssertEqual(files.rows.count, 150)
        restored.query = "note-149"
        try await waitUntil { !restored.searching }
        let hit = try XCTUnwrap(restored.hits.first)
        XCTAssertEqual(try restored.access(for: hit).url.lastPathComponent, "First")
        restored.query = ""
        XCTAssertTrue(branch.children[child] === files)
        branch.toggle(child)
        XCTAssertNil(branch.children[child])
        branch.toggle(child)
        files = try XCTUnwrap(branch.children[child])
        try await waitUntil { !files.busy }
        XCTAssertEqual(files.rows.count, 128)
        restored.forget(location)
        let reopened = WorkspaceModel(defaults: defaults, documents: local)
        XCTAssertEqual(reopened.locations.map(\.title), ["Slate Documents", "Second"])
    }

    @MainActor
    func testBackSkipsUnavailableFileAndKeepsItsTab() async throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let urls = ["first", "gone", "last"].map { dir.appendingPathComponent($0) }
        let model = EditorModel(history: NavigationHistory())
        for url in urls {
            try Data("text".utf8).write(to: url)
            model.open(url)
            try await waitUntil { !model.busy }
        }
        try FileManager.default.removeItem(at: urls[1])
        model.navigate(visit: try XCTUnwrap(model.history.back))
        try await waitUntil { !model.busy && model.file?.url.lastPathComponent == "first" }
        XCTAssertEqual(model.history.visibleTabs.map(\.name), ["first", "gone", "last"])
        model.navigate(visit: try XCTUnwrap(model.history.forward))
        try await waitUntil { !model.busy && model.file?.url.lastPathComponent == "last" }
    }

    @MainActor
    func testBundledOfflinePreviewRendersTableAndDiagram() async throws {
        let controller = PreviewController()
        let web = controller.makeView()
        let markup = "| a | b |\n| --- | --- |\n| 1 | 2 |\n\n```mermaid\nflowchart LR\n A --> B\n```"
        controller.update(web, PreviewContent(TextPage(text: markup), Appearance()))
        let query = "document.querySelectorAll('table tbody td').length === 2 && !!document.querySelector('.diagram svg')"
        for _ in 0..<180 {
            if (try? await web.evaluateJavaScript(query)) as? Bool == true { return }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        let body = try await web.evaluateJavaScript("document.body.innerText") as? String ?? ""
        XCTFail("Offline preview did not render table and diagram. \(body)")
    }
}
