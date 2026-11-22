import XCTest
import WebKit
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
        for index in [0, 1, 0, 1] { history.record(files[index]) }
        let prior = try XCTUnwrap(history.back)
        history.record(files[0], at: prior)
        XCTAssertEqual(history.tab(atVisit: try XCTUnwrap(history.forward))?.name, "b")
        history.record(files[2])
        history.remember(caret: 4099, preview: true)
        XCTAssertNil(history.forward)
        let restored = NavigationHistory(store: try XCTUnwrap(HistoryStore(url: database)))
        XCTAssertEqual(restored.tabs.map(\.name), ["a", "b", "c"])
        let current = try XCTUnwrap(restored.active)
        XCTAssertEqual(restored.tabs[current].caret, 4099)
        XCTAssertTrue(restored.tabs[current].preview)
        XCTAssertEqual(restored.tab(atVisit: try XCTUnwrap(restored.back))?.name, "a")
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
        try await waitUntil { !workspace.busy }
        XCTAssertEqual(Set(workspace.rows.map { $0.url.lastPathComponent }), ["notes.md", "log.txt"])
        workspace.contents = true
        workspace.query = "needle"
        try await waitUntil { !workspace.busy }
        let hit = try XCTUnwrap(workspace.rows.first)
        XCTAssertEqual(hit.offset, UInt64("# Notes\n🍏 ".utf8.count))
        model.open(second)
        model.openExternal(first)
        try await waitUntil { !model.busy }
        model.resumePendingOpen()
        try await waitUntil { !model.busy }
        XCTAssertEqual(model.file?.url, first)
        model.open(hit.url, workspace: workspace.root, offset: hit.offset, length: hit.matchLength)
        XCTAssertFalse(model.preview)
        XCTAssertEqual((model.page.text as NSString).substring(with: model.selection), "needle")
        let visit = try XCTUnwrap(model.history.back)
        model.navigate(visit: visit)
        try await waitUntil { !model.busy }
        XCTAssertEqual(model.file?.url.standardizedFileURL, second.standardizedFileURL)
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
