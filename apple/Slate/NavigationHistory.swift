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
    private var visits: [Int] = []
    private var position = -1
    private var showingFile = false

    private let store: HistoryStore?

    init(store: HistoryStore? = nil) {
        self.store = store
        if let store {
            (tabs, visits, position, showingFile) = store.load()
            for tab in tabs { tabIDs[tab.reference.url.standardizedFileURL] = tab.id }
        }
    }

    var currentVisit: Int? { showingFile ? position : nil }
    var active: Int? { showingFile ? visits[position] : nil }
    var back: Int? { showingFile ? (position > 0 ? position - 1 : nil) : (position >= 0 ? position : nil) }
    var forward: Int? { showingFile && position + 1 < visits.count ? position + 1 : nil }
    func tab(atVisit visit: Int) -> Tab { tabs[visits[visit]] }

    func remember(caret: UInt64, preview: Bool) {
        guard let active else { return }
        tabs[active].caret = caret
        tabs[active].preview = preview
        store?.remember(tabs[active])
    }

    func leaveFile() {
        objectWillChange.send()
        showingFile = false
        store?.setPosition(position, showing: false)
    }

    func record(_ file: ScopedURL, at visit: Int? = nil) {
        let key = file.url.standardizedFileURL
        objectWillChange.send()
        if let visit {
            position = visit
            showingFile = true
            let id = visits[visit]
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
            visits.removeSubrange((position + 1)..<visits.count)
            visits.append(id)
            position += 1
        }
        showingFile = true
        store?.record(tab: tabs[id], position: position, appended: appended)
    }
}
