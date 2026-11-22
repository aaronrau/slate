import Foundation
import Combine

struct FileRow: Identifiable {
    let url: URL
    let isDirectory: Bool
    var locationID: UUID?
    var offset: UInt64? = nil
    var matchLength = 0
    var excerpt = ""
    var id: String { url.path + "\u{0}" + String(offset ?? 0) }
}

private final class SearchJob {
    let handle: OpaquePointer
    var rows: [FileRow] = []
    private var seen: Set<String> = []
    private var locationID: UUID?
    private let lock = NSLock()
    private var cancelled = false
    var isCancelled: Bool { lock.withLock { cancelled } }
    init?() {
        guard let handle = slate_search_new() else { return nil }
        self.handle = handle
    }
    deinit { slate_search_free(handle) }
    func cancel() {
        lock.withLock { cancelled = true }
        slate_search_cancel(handle)
    }
    func run(_ location: FolderLocation, query: String, contents: Bool) throws -> Int64 {
        let access = try location.acquire()
        locationID = location.id
        return withExtendedLifetime(access) {
            slate_search_run(handle, access.url.path, query, contents ? 1 : 0, Unmanaged.passUnretained(self).toOpaque()) {
                context, path, offset, length, excerpt in
                guard let context, let path else { return }
                let job = Unmanaged<SearchJob>.fromOpaque(context).takeUnretainedValue()
                guard job.rows.count < 200 else { return }
                let url = URL(fileURLWithPath: String(cString: path))
                let row = FileRow(url: url, isDirectory: false, locationID: job.locationID, offset: length == 0 ? nil : offset,
                                  matchLength: length, excerpt: excerpt.map { String(cString: $0) } ?? "")
                if job.seen.insert(row.id).inserted { job.rows.append(row) }
            }
        }
    }
}

struct FolderLocation: Codable, Identifiable {
    let id: UUID
    let title: String
    let reference: FileReference
    let local: Bool
    func acquire() throws -> ScopedURL { try reference.acquire() }
}

private final class DirectoryCursor {
    let url: URL
    var iterator: FileManager.DirectoryEnumerator?
    var finished = false
    private var readError: Error?
    init(url: URL) { self.url = url }

    func next() throws -> [FileRow] {
        guard !finished else { return [] }
        return try coordinated(url) { url in
            let keys: [URLResourceKey] = [.isDirectoryKey, .isRegularFileKey, .isSymbolicLinkKey]
            if iterator == nil {
                iterator = FileManager.default.enumerator(at: url, includingPropertiesForKeys: keys,
                                                         options: [.skipsSubdirectoryDescendants]) { [weak self] _, error in
                    self?.readError = error
                    return false
                }
            }
            guard let iterator else { throw EditorFailure("Unable to list this folder.") }
            var rows: [FileRow] = []
            while rows.count < 128 {
                guard let entry = iterator.nextObject() as? URL else {
                    finished = true
                    if let readError { throw readError }
                    break
                }
                let info = try? entry.resourceValues(forKeys: Set(keys))
                guard info?.isSymbolicLink != true else { continue }
                // File providers can omit isRegularFile until an item is downloaded.
                let directory = info?.isDirectory ?? entry.hasDirectoryPath
                if !directory, info?.isRegularFile == false { continue }
                rows.append(FileRow(url: entry, isDirectory: directory))
            }
            return rows.sorted {
                if $0.isDirectory != $1.isDirectory { return $0.isDirectory }
                return $0.url.lastPathComponent.localizedStandardCompare($1.url.lastPathComponent) == .orderedAscending
            }
        }
    }
}

final class DirectoryBranch: ObservableObject {
    let url: URL
    let access: ScopedURL
    private let cursor: DirectoryCursor
    private var changedNames: Set<String> = []
    @Published private(set) var rows: [FileRow] = []
    @Published private(set) var children: [URL: DirectoryBranch] = [:]
    @Published private(set) var busy = false
    @Published private(set) var more = false
    @Published private(set) var error: String?

    init(url: URL, access: ScopedURL) {
        self.url = url
        self.access = access
        cursor = DirectoryCursor(url: url)
        loadMore()
    }

    func toggle(_ url: URL) {
        if children.removeValue(forKey: url) == nil {
            children[url] = DirectoryBranch(url: url, access: access)
        }
    }

    func apply(_ file: URL, exists: Bool) {
        if url.standardizedFileURL.path == file.deletingLastPathComponent().standardizedFileURL.path {
            let name = file.lastPathComponent
            if busy || more { changedNames.insert(name) }
            rows.removeAll { $0.url.lastPathComponent == name }
            if exists {
                let index = rows.firstIndex { !$0.isDirectory } ?? rows.endIndex
                rows.insert(FileRow(url: file, isDirectory: false), at: index)
            }
        } else {
            for child in children.values where file.pathComponents.starts(with: child.url.pathComponents) {
                child.apply(file, exists: exists)
            }
        }
    }

    func loadMore() {
        guard !busy, !cursor.finished else { return }
        busy = true
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self else { return }
            let result = Result { try self.cursor.next() }
            let more = !self.cursor.finished
            DispatchQueue.main.async {
                self.busy = false
                do {
                    self.rows += try result.get().filter { !self.changedNames.contains($0.url.lastPathComponent) }
                    self.more = more
                    if !more { self.changedNames.removeAll() }
                } catch {
                    self.more = false
                    self.error = error.localizedDescription
                }
            }
        }
    }
}

struct FileDestination: Identifiable {
    let location: FolderLocation
    let url: URL
    var id: URL { url }
    var label: String {
        let suffix = url.pathComponents.dropFirst(location.reference.url.pathComponents.count).joined(separator: "/")
        return suffix.isEmpty ? location.title : location.title + "/" + suffix
    }
}

final class WorkspaceModel: ObservableObject {
    @Published private(set) var locations: [FolderLocation] = []
    @Published private(set) var expanded: [UUID: DirectoryBranch] = [:]
    @Published private(set) var errors: [UUID: String] = [:]
    @Published private(set) var folderPicked = 0
    @Published var query = "" { didSet { if oldValue != query { search() } } }
    @Published var contents = false { didSet { if oldValue != contents { search() } } }
    @Published private(set) var hits: [FileRow] = []
    @Published private(set) var searching = false
    @Published private(set) var message: String?

    private let defaults: UserDefaults?
    private let savedFolders = "Slate.workspaceFolders"
    private let lastFolder = "Slate.activeWorkspaceFolder"
    private let queue = DispatchQueue(label: "org.slateeditor.search", qos: .userInitiated)
    private var searchJob: SearchJob?

    static func applicationWorkspace() -> WorkspaceModel {
        #if os(iOS)
        let local = try? FileManager.default.url(for: .documentDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
        #else
        let local: URL? = nil
        #endif
        return WorkspaceModel(defaults: .standard, documents: local)
    }

    init(defaults: UserDefaults? = nil, documents: URL? = nil) {
        self.defaults = defaults
        if let data = defaults?.data(forKey: savedFolders),
           let folders = try? JSONDecoder().decode([FolderLocation].self, from: data) {
            locations = folders
        }
        if let documents {
            let local = ScopedURL(documents)
            locations.insert(FolderLocation(id: UUID(), title: "Slate Documents", reference: local.reference, local: true), at: 0)
        }
        let last = defaults?.string(forKey: lastFolder)
        if let location = locations.first(where: { $0.id.uuidString == last }) ?? locations.first {
            expand(location)
        }
    }

    func open(folder: URL) {
        let access = ScopedURL(folder)
        let existing = locations.firstIndex { $0.reference.url.standardizedFileURL.path == folder.standardizedFileURL.path }
        let old = existing.map { locations[$0] }
        let location = FolderLocation(id: old?.id ?? UUID(), title: old?.local == true ? "Slate Documents" : folder.lastPathComponent,
                                      reference: access.reference, local: old?.local ?? false)
        if let existing { locations[existing] = location } else { locations.append(location) }
        persist()
        defaults?.set(location.id.uuidString, forKey: lastFolder)
        query = ""
        expand(location, access: access)
        folderPicked += 1
    }

    func includeGrant(for file: ScopedURL?) {
        // Documents from older sessions can carry the original folder bookmark.
        // An individually picked document only grants access to that file.
        guard let grant = file?.parent,
              !locations.contains(where: { $0.reference.url.standardizedFileURL.path == grant.url.standardizedFileURL.path }) else { return }
        let folder = FolderLocation(id: UUID(), title: grant.url.lastPathComponent, reference: grant.reference, local: false)
        locations.append(folder)
        expand(folder, access: grant)
    }

    func toggle(_ location: FolderLocation) {
        if expanded.removeValue(forKey: location.id) == nil { expand(location) }
    }

    private func expand(_ location: FolderLocation, access: ScopedURL? = nil) {
        do {
            let grant = try access ?? location.acquire()
            errors.removeValue(forKey: location.id)
            expanded[location.id] = DirectoryBranch(url: grant.url, access: grant)
            // Re-encode a resolved bookmark so moved folders and stale grants
            // continue working on the next launch.
            if !location.local, let index = locations.firstIndex(where: { $0.id == location.id }) {
                locations[index] = FolderLocation(id: location.id, title: grant.url.lastPathComponent,
                                                  reference: grant.reference, local: false)
                persist()
            }
        } catch {
            errors[location.id] = "Choose this folder again to restore access."
        }
    }

    func forget(_ location: FolderLocation) {
        guard !location.local else { return }
        expanded.removeValue(forKey: location.id)
        locations.removeAll { $0.id == location.id }
        errors.removeValue(forKey: location.id)
        persist()
        search()
    }

    private func persist() {
        if let data = try? JSONEncoder().encode(locations.filter { !$0.local }) { defaults?.set(data, forKey: savedFolders) }
    }

    func access(for hit: FileRow) throws -> ScopedURL {
        guard let location = locations.first(where: { $0.id == hit.locationID }) else {
            throw EditorFailure("Choose the folder again to open this result.")
        }
        return try expanded[location.id]?.access ?? location.acquire()
    }

    func destinations(including extra: FileDestination? = nil) -> [FileDestination] {
        var result: [FileDestination] = []
        for location in locations {
            let branch = expanded[location.id]
            result.append(FileDestination(location: location, url: branch?.url ?? location.reference.url))
            var stack = Array(branch?.children.values.map { $0 } ?? [])
            while let node = stack.popLast() {
                result.append(FileDestination(location: location, url: node.url))
                stack.append(contentsOf: node.children.values)
            }
        }
        if let extra, !result.contains(where: { $0.url == extra.url }) { result.append(extra) }
        var seen: Set<String> = []
        return result.filter { seen.insert($0.url.standardizedFileURL.path).inserted }
                     .sorted { $0.label.localizedStandardCompare($1.label) == .orderedAscending }
    }

    func applyChange(to file: URL, exists: Bool, destination: FileDestination? = nil) {
        if let destination {
            let location = destination.location
            if expanded[location.id] == nil { expand(location) }
            var node = expanded[location.id]
            if let root = node, destination.url.pathComponents.starts(with: root.url.pathComponents) {
                for component in destination.url.pathComponents.dropFirst(root.url.pathComponents.count) {
                    guard let parent = node else { break }
                    let child = parent.url.appendingPathComponent(component, isDirectory: true)
                    if parent.children[child] == nil { parent.toggle(child) }
                    node = parent.children[child]
                }
            }
        }
        for branch in expanded.values { branch.apply(file, exists: exists) }
        search()
    }

    func refresh() {
        for location in locations where expanded[location.id] != nil || errors[location.id] != nil {
            expand(location)
        }
        search()
    }

    private func search() {
        searchJob?.cancel()
        searchJob = nil
        searching = false
        hits = []
        message = nil
        guard !query.isEmpty, !locations.isEmpty, let job = SearchJob() else { return }
        searchJob = job
        searching = true
        let locations = self.locations, query = self.query, contents = self.contents
        queue.asyncAfter(deadline: .now() + .milliseconds(180)) {
            var errors: Int64 = 0
            for location in locations {
                if job.isCancelled || job.rows.count >= 200 { break }
                do { errors += max(0, try job.run(location, query: query, contents: contents)) }
                catch { errors += 1 }
            }
            DispatchQueue.main.async {
                guard self.searchJob === job else { return }
                self.hits = job.rows
                self.searching = false
                if job.rows.count == 200 { self.message = "Showing the first 200 matches." }
                else if errors > 0 { self.message = "\(errors) unreadable entries" }
            }
        }
    }
}
