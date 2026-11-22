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
        XCTAssertEqual(files.rows.count, 22)
        XCTAssertEqual(files.count, 150)
        files.previous()
        try await waitUntil { !files.busy }
        XCTAssertEqual(files.rows.count, 128)
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
    func testLargeDirectoryMetadataPagesAndMixedMediaSearch() async throws {
        let fm = FileManager.default
        let base = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let root = base.appendingPathComponent("Work Bench Audio")
        let nested = root.appendingPathComponent("Nested")
        let db = base.appendingPathComponent("names.sqlite")
        try fm.createDirectory(at: nested, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: base) }
        for n in 0..<2048 {
            let ext = n % 3 == 0 ? "wav" : "txt"
            try Data().write(to: root.appendingPathComponent("take-\(n).\(ext)"))
        }
        let term = "transcript needle"
        try Data(term.utf8).write(to: nested.appendingPathComponent("ignored.WAV"))
        try (Data([0]) + Data(term.utf8)).write(to: nested.appendingPathComponent("opaque"))
        try Data(("line " + term).utf8).write(to: nested.appendingPathComponent("notes.txt"))
        let access = ScopedURL(root)
        let cold = DirectoryCursor(url: root, access: access, database: db)
        let first = try cold.page(0)
        XCTAssertEqual(first.count, 128)
        XCTAssertNil(cold.count)
        XCTAssertEqual(cold.scanned, 128)
        while cold.count == nil { try cold.warm() }
        XCTAssertEqual(cold.count, 2049)
        let warm = DirectoryCursor(url: root, access: access, database: db)
        XCTAssertEqual(try warm.page(0).map(\.id), first.map(\.id))
        XCTAssertTrue(warm.cached)
        XCTAssertEqual(try warm.page(15).count, 128)
        XCTAssertEqual(try warm.page(16).count, 1)
        XCTAssertEqual(warm.scanned, 0)

        let model = WorkspaceModel(documents: root, database: db)
        model.query = "nested/igno"
        try await waitUntil { !model.searching }
        XCTAssertEqual(model.hits.map { $0.url.lastPathComponent }, ["ignored.WAV"])
        model.contents = true
        model.query = term
        try await waitUntil { !model.searching }
        XCTAssertEqual(model.hits.map { $0.url.lastPathComponent }, ["notes.txt"])
        XCTAssertEqual(model.hits.first?.offset, 5)

        try fm.removeItem(at: root.appendingPathComponent("take-0.wav"))
        let updated = DirectoryCursor(url: root, access: access, database: db)
        _ = try updated.page(0)
        XCTAssertFalse(updated.cached)
        while updated.count == nil { try updated.warm() }
        XCTAssertEqual(updated.count, 2048)
        try fm.moveItem(at: root, to: base.appendingPathComponent("Moved"))
        XCTAssertThrowsError(try DirectoryCursor(url: root, access: access, database: db).page(0))
    }

    @MainActor
    func testShowInFolderUsesOnlyGrantedParentsAndRevealsTheNestedFile() async throws {
        let fm = FileManager.default
        let base = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let root = base.appendingPathComponent("Work Bench Audio")
        let nested = root.appendingPathComponent("Nested")
        try fm.createDirectory(at: nested, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: base) }
        let file = ScopedURL(nested.appendingPathComponent("transcript.txt"))
        try Data().write(to: file.url)
        let workspace = WorkspaceModel()
        var requested: URL?
        workspace.showInFolder(file) { requested = $0 }
        try await waitUntil { requested != nil }
        XCTAssertEqual(requested?.standardizedFileURL.path, nested.standardizedFileURL.path)
        XCTAssertTrue(workspace.locations.isEmpty)
        let other = base.appendingPathComponent("Other")
        try fm.createDirectory(at: other, withIntermediateDirectories: false)
        workspace.open(folder: other)
        workspace.open(folder: root)
        let location = try XCTUnwrap(workspace.locations.first(where: { $0.title == "Work Bench Audio" }))
        XCTAssertEqual(workspace.sidebarLocations.first?.title, "Other")
        workspace.showInFolder(file) { _ in XCTFail("The previously granted root covers this file.") }
        try await waitUntil { workspace.revealedFile == file.url }
        XCTAssertEqual(workspace.sidebarLocations.first?.id, location.id)
        let parent = try XCTUnwrap(workspace.expanded[location.id]?.children.values.first)
        XCTAssertEqual(parent.displayedRows.first?.url.lastPathComponent, "transcript.txt")
        workspace.forget(location)
        requested = nil
        let formerGrant = ScopedURL(file.url, parent: ScopedURL(root))
        workspace.showInFolder(formerGrant) { requested = $0 }
        try await waitUntil { requested != nil }
        XCTAssertEqual(workspace.sidebarLocations.map(\.title), ["Other"])
    }

    func testExclusiveCreationAndDeletionOnlyOfUnchangedRegularFiles() throws {
        let base = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let folder = base.appendingPathComponent("Workspace")
        let outside = base.appendingPathComponent("Other")
        defer { try? FileManager.default.removeItem(at: base) }
        for url in [folder, outside] { try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true) }
        let grant = ScopedURL(folder)
        for name in ["", " ", ".", "..", "../escape", "/outside", "dir/name", "dir\\name", "null\u{0}suffix"] {
            XCTAssertThrowsError(try createEmptyFile(named: name, in: folder, grant: grant))
        }
        let link = folder.appendingPathComponent("link")
        try FileManager.default.createSymbolicLink(at: link, withDestinationURL: outside)
        XCTAssertThrowsError(try createEmptyFile(named: "escape", in: link, grant: grant))
        XCTAssertFalse(FileManager.default.fileExists(atPath: outside.appendingPathComponent("escape").path))
        XCTAssertThrowsError(try FileDeletion(grant))
        let file = try createEmptyFile(named: "unique.txt", in: folder, grant: grant)
        try Data("existing".utf8).write(to: file.url)
        XCTAssertThrowsError(try createEmptyFile(named: "unique.txt", in: folder, grant: grant))
        XCTAssertEqual(try Data(contentsOf: file.url), Data("existing".utf8))
        let pending = try FileDeletion(file)
        try FileManager.default.removeItem(at: file.url)
        try FileManager.default.createDirectory(at: file.url, withIntermediateDirectories: false)
        let child = file.url.appendingPathComponent("keep")
        try Data("kept".utf8).write(to: child)
        XCTAssertThrowsError(try pending.remove())
        XCTAssertEqual(try Data(contentsOf: child), Data("kept".utf8))
    }

    @MainActor
    func testDeletingCurrentFileUpdatesTreeSearchAndPersistedNavigation() async throws {
        let base = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let folder = base.appendingPathComponent("workspace")
        defer { try? FileManager.default.removeItem(at: base) }
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let database = base.appendingPathComponent("history.sqlite")
        let history = NavigationHistory(store: try XCTUnwrap(HistoryStore(url: database)))
        let model = EditorModel(history: history)
        let workspace = WorkspaceModel(documents: folder)
        let choice = try XCTUnwrap(workspace.destinations().first)
        let root = try XCTUnwrap(workspace.expanded[choice.location.id])
        let first = try createEmptyFile(named: "keep", in: folder, grant: root.access)
        model.open(first.url)
        try await waitUntil { !model.busy }
        model.createFile(named: "test.txt", destination: choice, workspace: workspace) { _ in }
        try await waitUntil { !model.busy && model.title == "test.txt" }
        let file = try XCTUnwrap(model.file)
        model.edit("unsaved", caret: NSRange(location: 3, length: 0), composing: false)
        workspace.query = "test.txt"
        try await waitUntil { !workspace.searching }
        XCTAssertEqual(workspace.hits.count, 1)
        model.requestDeletion(file)
        try await waitUntil { model.deletion != nil }
        model.deletion = nil
        XCTAssertTrue(model.dirty)
        XCTAssertTrue(FileManager.default.fileExists(atPath: file.url.path))
        model.requestDeletion(file)
        try await waitUntil { model.deletion != nil }
        model.confirmDeletion(try XCTUnwrap(model.deletion), workspace: workspace)
        // The folder's rows reload on the directory queue after the deletion.
        try await waitUntil { !model.busy && !workspace.searching && !root.busy }
        XCTAssertNil(model.errorMessage)
        XCTAssertNil(model.file)
        XCTAssertFalse(model.dirty)
        XCTAssertFalse(FileManager.default.fileExists(atPath: file.url.path))
        XCTAssertTrue(workspace.hits.isEmpty)
        XCTAssertFalse(root.rows.contains { $0.url.lastPathComponent == "test.txt" })
        let restored = NavigationHistory(store: try XCTUnwrap(HistoryStore(url: database)))
        XCTAssertNil(restored.currentVisit)
        XCTAssertEqual(restored.tabCount, 1)
        XCTAssertEqual(restored.tab(atVisit: try XCTUnwrap(restored.back))?.name, "keep")
        XCTAssertEqual(restored.visibleTabs.map(\.name), ["keep"])
    }

    func testPagingSparseHistoryIDsAfterDeletion() throws {
        let base = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: base) }
        try FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        let history = NavigationHistory()
        for i in 0..<100 {
            let file = ScopedURL(base.appendingPathComponent(String(i)))
            try Data().write(to: file.url)
            try history.record(file)
            if i % 3 == 1 { try history.remove(FileDeletion(file)) }
        }
        // Add a dense range then remove holes so stable IDs span multiple strips.
        for i in 100..<180 {
            let file = ScopedURL(base.appendingPathComponent(String(i)))
            try Data().write(to: file.url)
            try history.record(file)
        }
        for i in stride(from: 103, to: 172, by: 3) { try history.remove(FileDeletion(ScopedURL(base.appendingPathComponent(String(i))))) }
        let expected = Set((0..<180).filter { ($0 >= 100 || $0 % 3 != 1) && !($0 >= 103 && $0 < 172 && ($0 - 103) % 3 == 0) }.map(String.init))
        var seen: [String] = []
        var cursor = Int.max
        while true {
            let page = history.tabPage(starting: cursor, backward: true)
            guard let last = page.last else { break }
            seen += page.map(\.name)
            cursor = last.id - 1
        }
        XCTAssertEqual(seen.count, expected.count)
        XCTAssertEqual(Set(seen), expected)
        while history.hasEarlierTabs { history.shiftStrip(forward: false) }
        var visible = history.visibleTabs.map(\.name)
        while history.hasLaterTabs {
            history.shiftStrip(forward: true)
            visible += history.visibleTabs.map(\.name)
        }
        XCTAssertEqual(visible.count, expected.count)
        XCTAssertEqual(Set(visible), expected)

        let oldest = try XCTUnwrap(history.tabPage(starting: 0, limit: 1).first)
        try history.remove(FileDeletion(ScopedURL(URL(fileURLWithPath: oldest.path))))
        let nearest = history.tabPage(starting: oldest.id + 1)
        XCTAssertFalse(nearest.isEmpty)
        XCTAssertEqual(history.openedFiles(endingAt: oldest.id).map(\.id), nearest.reversed().map(\.id))
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

    @MainActor
    func testPreviewTableColumnsKeepWordsWholeAndFitLongPaths() async throws {
        let controller = PreviewController()
        let web = controller.makeView()
        web.frame = CGRect(x: 0, y: 0, width: 420, height: 400)
        let markup = """
        | ID | Canonical report and exact cycles | Tested change / scope | Where |
        | --- | --- | --- | --- |
        | 01 | Original Python replay | Separate actor/order/correction/hypothetical/unknown checks. | https://example.com/a/really/long/path/to/configuration-reference.html |
        """
        controller.update(web, PreviewContent(TextPage(text: markup), Appearance(), fontSize: 13))
        // Each word's own width must fit its cell (no word split across lines), and the
        // paths and URL wrap at their separators, so the table fits the page.
        let query = """
        (() => {
          const table = document.querySelector('table'); if (!table) return 'pending';
          const box = table.parentElement;
          const words = ['Canonical', 'Original', 'Python', 'replay', 'Tested', 'change'];
          for (const cell of table.querySelectorAll('th, td')) {
            const range = document.createRange();
            for (const node of cell.childNodes) {
              if (node.nodeType !== 3) continue;
              for (const word of words) {
                const at = node.data.indexOf(word); if (at < 0) continue;
                range.setStart(node, at); range.setEnd(node, at + word.length);
                if (range.getClientRects().length !== 1) return 'split ' + word;
              }
            }
          }
          if (!table.querySelector('wbr')) return 'no break points';
          return table.scrollWidth <= box.clientWidth + 1 ? 'ok' : 'wide ' + table.scrollWidth + '>' + box.clientWidth;
        })()
        """
        var result = ""
        for _ in 0..<100 {
            result = (try? await web.evaluateJavaScript(query)) as? String ?? ""
            if result != "pending" && !result.isEmpty { break }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        XCTAssertEqual(result, "ok")
    }

    @MainActor
    func testPreviewDiagramsZoomAndAcceptSemicolonsInSequenceMessages() async throws {
        let controller = PreviewController()
        let web = controller.makeView()
        web.frame = CGRect(x: 0, y: 0, width: 1000, height: 600)
        let markup = """
        # Diagrams

        ```mermaid
        flowchart LR
          A[Start] --> B{Ready?} --> C[Done]
        ```

        ```mermaid
        sequenceDiagram
          participant U as User
          participant W as Guarded worker
          U->>W: Actual history and draft
          W->>W: Project existing goals; append goal/feedback references once; seal bounded payload
          alt Optional guidance ready and matching
            W-->>U: Bounded selected payload
          else Missing, stale or rejected
            U->>U: Immediate native fallback with current completion state
          end
        ```
        """
        controller.update(web, PreviewContent(TextPage(text: markup), Appearance(), fontSize: 13))
        let ready = "document.querySelectorAll('.diagram > .diagram-view > svg').length === 2"
            + " || document.querySelectorAll('.diagram details').length > 0"
        var rendered = false
        for _ in 0..<180 {
            if (try? await web.evaluateJavaScript(ready)) as? Bool == true { rendered = true; break }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        let body = try await web.evaluateJavaScript("document.body.innerText") as? String ?? ""
        XCTAssertTrue(rendered, "Diagrams did not render. \(body)")
        XCTAssertFalse(body.contains("Could not render"), body)
        let probe = """
        (() => {
          const views = Array.from(document.querySelectorAll('.diagram-view'));
          const view = views[0], tools = view.parentElement;
          const result = {
            svgs: document.querySelectorAll('.diagram svg').length,
            controls: document.querySelectorAll('.diagram-tools button[data-zoom]').length,
            labelled: Array.from(document.querySelectorAll('.diagram-tools button')).every(b => b.getAttribute('aria-label')),
            sequenceText: views[1].querySelector('svg').textContent,
            initial: +view.dataset.scale
          };
          tools.querySelector('[data-zoom=in]').click();
          result.zoomedIn = +view.dataset.scale;
          tools.querySelector('[data-zoom=out]').click();
          tools.querySelector('[data-zoom=out]').click();
          result.zoomedOut = +view.dataset.scale;
          tools.querySelector('[data-zoom=reset]').click();
          result.reset = +view.dataset.scale;
          tools.querySelector('[data-zoom=fit]').click();
          result.fit = +view.dataset.scale;
          result.fitExpected = view.clientWidth / view.querySelector('svg').viewBox.baseVal.width;
          for (let i = 0; i < 20; i++) tools.querySelector('[data-zoom=in]').click();
          result.max = +view.dataset.scale;
          const wheel = ctrlKey => view.dispatchEvent(new WheelEvent('wheel', {ctrlKey, deltaY: -10, bubbles: true, cancelable: true}));
          result.pinchHandled = !wheel(true);
          result.scrollHandled = !wheel(false);
          return JSON.stringify(result);
        })()
        """
        let output = try await web.evaluateJavaScript(probe)
        let json = try XCTUnwrap(output as? String)
        let result = try XCTUnwrap(try JSONSerialization.jsonObject(with: Data(json.utf8)) as? [String: Any])
        XCTAssertEqual(result["svgs"] as? Int, 2)
        XCTAssertEqual(result["controls"] as? Int, 8)
        XCTAssertEqual(result["labelled"] as? Bool, true)
        XCTAssertTrue((result["sequenceText"] as? String ?? "").contains("Project existing goals; append"), "\(result)")
        let initial = try XCTUnwrap(result["initial"] as? Double)
        XCTAssertEqual(initial, 1, accuracy: 0.001, "A diagram that fits starts at its natural size")
        XCTAssertEqual(try XCTUnwrap(result["zoomedIn"] as? Double), 1.25, accuracy: 0.001)
        XCTAssertEqual(try XCTUnwrap(result["zoomedOut"] as? Double), 0.8, accuracy: 0.001)
        XCTAssertEqual(try XCTUnwrap(result["reset"] as? Double), 1, accuracy: 0.001)
        XCTAssertEqual(try XCTUnwrap(result["max"] as? Double), 4, accuracy: 0.001)
        XCTAssertEqual(result["pinchHandled"] as? Bool, true)
        XCTAssertEqual(result["scrollHandled"] as? Bool, false, "Plain wheel scrolling must not be hijacked")
        let fitExpected = try XCTUnwrap(result["fitExpected"] as? Double)
        XCTAssertEqual(try XCTUnwrap(result["fit"] as? Double), min(4, max(0.25, fitExpected)), accuracy: 0.001)

        // Too wide for its box: start at fit-to-width, but never with labels under 10px.
        web.frame = CGRect(x: 0, y: 0, width: 260, height: 600)
        controller.update(web, PreviewContent(TextPage(text: markup + "\n"), Appearance(), fontSize: 13))
        let narrow = """
        (() => {
          const view = document.querySelector('.diagram-view');
          if (!view || view.dataset.scale === '4' || document.querySelectorAll('.diagram-view').length !== 2) return null;
          const width = view.querySelector('svg').viewBox.baseVal.width;
          return JSON.stringify({scale: +view.dataset.scale, expected: Math.min(1, Math.max(view.clientWidth / width, 10 / 13)),
                                 wide: width > view.clientWidth, pannable: view.classList.contains('pannable')});
        })()
        """
        var narrowResult: [String: Any] = [:]
        for _ in 0..<180 {
            if let text = (try? await web.evaluateJavaScript(narrow)) as? String,
               let object = try JSONSerialization.jsonObject(with: Data(text.utf8)) as? [String: Any] {
                narrowResult = object
                break
            }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        XCTAssertEqual(narrowResult["wide"] as? Bool, true, "\(narrowResult)")
        let narrowScale = try XCTUnwrap(narrowResult["scale"] as? Double)
        XCTAssertEqual(narrowScale, try XCTUnwrap(narrowResult["expected"] as? Double), accuracy: 0.001)
        XCTAssertGreaterThanOrEqual(narrowScale, 10.0 / 13 - 0.001)
        XCTAssertEqual(narrowResult["pannable"] as? Bool, true)
    }
}
