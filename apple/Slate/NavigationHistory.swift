import Foundation
import Combine

// A bookmark retains permission to reacquire access without keeping grant slots
// or backing file descriptors open for every tab in a long session.
final class FileReference: Codable {
    let url: URL
    private let bookmark: Data?
    private let folder: FileReference?
    private let relativePath: String?

    init(_ access: ScopedURL) {
        url = access.url
        let folderURL = access.parent?.url.standardizedFileURL.path
        if let folderURL, url.standardizedFileURL.path.hasPrefix(folderURL + "/") {
            folder = access.parent?.reference
            relativePath = String(url.standardizedFileURL.path.dropFirst(folderURL.count + 1))
            bookmark = nil
        } else {
            folder = nil
            relativePath = nil
            #if os(macOS)
            let options: URL.BookmarkCreationOptions = .withSecurityScope
            #else
            let options: URL.BookmarkCreationOptions = .minimalBookmark
            #endif
            bookmark = try? url.bookmarkData(options: options, includingResourceValuesForKeys: nil, relativeTo: nil)
        }
    }

    func acquire() throws -> ScopedURL {
        if let folder, let relativePath {
            let parent = try folder.acquire()
            return ScopedURL(parent.url.appendingPathComponent(relativePath), parent: parent)
        }
        guard let bookmark else { return ScopedURL(url) }
        var stale = false
        #if os(macOS)
        let options: URL.BookmarkResolutionOptions = [.withSecurityScope, .withoutUI]
        #else
        let options: URL.BookmarkResolutionOptions = []
        #endif
        let resolved = try URL(resolvingBookmarkData: bookmark, options: options, relativeTo: nil, bookmarkDataIsStale: &stale)
        return ScopedURL(resolved)
    }
}

final class NavigationHistory: ObservableObject {
    struct Tab: Identifiable {
        let id: Int
        let path: String
        let caret: UInt64
        let preview: Bool
        var name: String { (path as NSString).lastPathComponent }
    }
    static let stripSize = 32
    private let store: HistoryStore
    private(set) var visibleTabs: [Tab] = []
    private(set) var tabCount: Int
    private(set) var position: Int
    private(set) var active: Int?
    private var stripStart = 0
    private(set) var back: Int?
    private(set) var forward: Int?

    init(store: HistoryStore? = nil) {
        self.store = store ?? .inMemory()
        tabCount = self.store.tabCount
        position = self.store.position
        active = self.store.showing ? self.store.tabID(at: position) : nil
        loadStrip(containing: active ?? 0)
        updateArrows()
    }

    var currentVisit: Int? { active == nil ? nil : position }
    func tab(atVisit visit: Int) -> Tab? {
        guard let id = store.tabID(at: visit) else { return nil }
        return tab(id: id)
    }
    func tabPage(starting index: Int) -> [Tab] { store.tabs(from: index, limit: Self.stripSize) }

    func tab(id: Int) -> Tab? {
        guard let tab = store.tabs(from: id, limit: 1).first, tab.id == id else { return nil }
        return tab
    }
    func acquire(_ tab: Tab) throws -> ScopedURL {
        guard let reference = store.file(for: tab.id) else { throw EditorFailure("Cannot read the saved location of “\(tab.name)”.") }
        return try reference.acquire()
    }

    var hasEarlierTabs: Bool { stripStart > 0 }
    var hasLaterTabs: Bool { stripStart + Self.stripSize < tabCount }
    func shiftStrip(forward: Bool) {
        objectWillChange.send()
        loadStrip(containing: min(tabCount - 1, max(0, stripStart + (forward ? Self.stripSize : -Self.stripSize))))
    }
    private func loadStrip(containing id: Int) {
        stripStart = max(0, id / Self.stripSize * Self.stripSize)
        visibleTabs = store.tabs(from: stripStart, limit: Self.stripSize)
    }
    private func updateArrows() {
        back = store.visit(starting: position - (active == nil ? 0 : 1), backward: true)
        forward = active == nil ? nil : store.visit(starting: position + 1, backward: false)
    }

    func markUnavailable(_ id: Int) {
        objectWillChange.send()
        store.markUnavailable(id)
        updateArrows()
    }
    func nextAvailable(after visit: Int, backward: Bool) -> Int? {
        store.visit(starting: visit + (backward ? -1 : 1), backward: backward)
    }
    func remember(caret: UInt64, preview: Bool) {
        if let active { store.remember(active, caret: caret, preview: preview) }
    }
    func leaveFile() {
        objectWillChange.send()
        active = nil
        store.setPosition(position, showing: false)
        updateArrows()
    }

    func record(_ file: ScopedURL, at visit: Int? = nil) throws {
        let id = visit.flatMap { store.tabID(at: $0) } ?? store.tabID(path: file.url.standardizedFileURL.path) ?? tabCount
        let append = visit == nil && active != id
        let next = visit ?? (append ? position + 1 : position)
        try store.record(file: file, id: id, position: next, appended: append)
        objectWillChange.send()
        active = id
        position = next
        tabCount = max(tabCount, id + 1)
        loadStrip(containing: id)
        updateArrows()
    }
}
