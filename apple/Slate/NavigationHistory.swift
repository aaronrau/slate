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
        var reference: FileReference
        var caret: UInt64 = 0
        var preview = false
        var name: String { reference.url.lastPathComponent }
    }
    private(set) var tabs: [Tab] = []
    private var tabIDs: [URL: Int] = [:]
    // Visits stay indexed in SQLite. The array is used only for an in-memory session.
    private var volatileVisits: [Int] = []
    private var last = -1
    private(set) var active: Int?
    private var position = -1

    private let store: HistoryStore?

    init(store: HistoryStore? = nil) {
        self.store = store
        if let store {
            let restored = store.load()
            (tabs, last, position) = (restored.0, restored.1, restored.2)
            if restored.3, let id = store.tabID(at: position), tabs.indices.contains(id) { active = id }
            for tab in tabs { tabIDs[tab.reference.url.standardizedFileURL] = tab.id }
        }
    }

    var currentVisit: Int? { active != nil ? position : nil }
    var back: Int? { active != nil ? (position > 0 ? position - 1 : nil) : (position >= 0 ? position : nil) }
    var forward: Int? { active != nil && position < last ? position + 1 : nil }
    func tab(atVisit visit: Int) -> Tab? {
        let id = store?.tabID(at: visit) ?? (volatileVisits.indices.contains(visit) ? volatileVisits[visit] : nil)
        guard let id, tabs.indices.contains(id) else { return nil }
        return tabs[id]
    }

    func remember(caret: UInt64, preview: Bool) {
        guard let active else { return }
        tabs[active].caret = caret
        tabs[active].preview = preview
        store?.remember(tabs[active])
    }

    func leaveFile() {
        objectWillChange.send()
        active = nil
        store?.setPosition(position, showing: false)
    }

    func record(_ file: ScopedURL, at visit: Int? = nil) {
        let key = file.url.standardizedFileURL
        objectWillChange.send()
        if let visit {
            guard let id = tab(atVisit: visit)?.id else { return }
            position = visit
            active = id
            tabs[id].reference = file.reference
            tabIDs[key] = id
            store?.record(tab: tabs[id], position: position, appended: false)
            return
        }
        let id: Int
        if let existing = tabIDs[key] {
            id = existing
            tabs[id].reference = file.reference
        } else {
            id = tabs.count
            tabs.append(Tab(id: id, reference: file.reference))
            tabIDs[key] = id
        }
        let appended = active != id
        if appended {
            position += 1
            last = position
            if store == nil {
                volatileVisits.removeSubrange(position..<volatileVisits.count)
                volatileVisits.append(id)
            }
        }
        active = id
        store?.record(tab: tabs[id], position: position, appended: appended)
    }
}
