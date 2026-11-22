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
        XCTAssertEqual(history.tab(atVisit: try XCTUnwrap(history.forward)).name, "b")
        history.record(files[2])
        history.remember(caret: 4099, preview: true)
        XCTAssertNil(history.forward)
        let restored = NavigationHistory(store: try XCTUnwrap(HistoryStore(url: database)))
        XCTAssertEqual(restored.tabs.map(\.name), ["a", "b", "c"])
        let current = try XCTUnwrap(restored.active)
        XCTAssertEqual(restored.tabs[current].caret, 4099)
        XCTAssertTrue(restored.tabs[current].preview)
        XCTAssertEqual(restored.tab(atVisit: try XCTUnwrap(restored.back)).name, "a")
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
