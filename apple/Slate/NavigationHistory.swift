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
    func tabPage(starting id: Int, backward: Bool = false, limit: Int = NavigationHistory.stripSize) -> [Tab] {
        store.tabs(from: id, limit: limit, backward: backward)
    }

    func openedFiles(endingAt id: Int) -> [Tab] {
        let page = tabPage(starting: id, backward: true)
        // Deleting the last row of the oldest page leaves the view's anchor
        // below every surviving ID. Show the nearest remaining page.
        if page.isEmpty && tabCount > 0 { return tabPage(starting: id).reversed() }
        return page
    }

    func tab(id: Int) -> Tab? {
        guard let tab = store.tabs(from: id, limit: 1).first, tab.id == id else { return nil }
        return tab
    }
    func acquire(_ tab: Tab) throws -> ScopedURL {
        guard let reference = store.file(for: tab.id) else { throw EditorFailure("Cannot read the saved location of “\(tab.name)”.") }
        return try reference.acquire()
    }

    private(set) var hasEarlierTabs = false
    private(set) var hasLaterTabs = false
    func shiftStrip(forward: Bool) {
        objectWillChange.send()
        if forward, let last = visibleTabs.last { showStrip(store.tabs(from: last.id + 1, limit: Self.stripSize)) }
        else if !forward, let first = visibleTabs.first {
            showStrip(store.tabs(from: first.id - 1, limit: Self.stripSize, backward: true).reversed())
        }
    }
    private func loadStrip(containing id: Int) {
        let start = max(0, id / Self.stripSize * Self.stripSize)
        let tabs = store.tabs(from: start, limit: Self.stripSize)
        if tabs.isEmpty { showStrip(store.tabs(from: start, limit: Self.stripSize, backward: true).reversed()) }
        else { showStrip(tabs) }
    }
    private func showStrip(_ tabs: [Tab]) {
        visibleTabs = tabs
        stripStart = tabs.first?.id ?? 0
        hasEarlierTabs = !store.tabs(from: stripStart - 1, limit: 1, backward: true).isEmpty
        hasLaterTabs = !store.tabs(from: (tabs.last?.id ?? -1) + 1, limit: 1).isEmpty
    }

    func remove(_ file: FileDeletion) throws {
        let ids = try store.remove(path: file.access.url.standardizedFileURL.path, tabID: file.tabID, active: active, position: position)
        guard !ids.isEmpty else { return }
        objectWillChange.send()
        tabCount -= ids.count
        if let active, ids.contains(active) { self.active = nil }
        loadStrip(containing: active ?? stripStart)
        updateArrows()
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
        let existing = visit.flatMap { store.tabID(at: $0) } ?? store.tabID(path: file.url.standardizedFileURL.path)
        let id = existing ?? store.nextTabID
        let append = visit == nil && active != id
        let next = visit ?? (append ? position + 1 : position)
        try store.record(file: file, id: id, position: next, appended: append)
        objectWillChange.send()
        active = id
        position = next
        if existing == nil { tabCount += 1 }
        loadStrip(containing: id)
        updateArrows()
    }
}
