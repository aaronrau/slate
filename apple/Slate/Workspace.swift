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
    var progress: (([FileRow]) -> Void)?
    private var lastUpdate = Date.distantPast
    func run(_ location: FolderLocation, database: URL?, query: String, contents: Bool) throws -> Int64 {
        let access = try location.acquire()
        locationID = location.id
        return withExtendedLifetime(access) {
            slate_search_run(handle, access.url.path, database?.path ?? "", query, contents ? 1 : 0, 200 - rows.count, Unmanaged.passUnretained(self).toOpaque()) {
                context, path, offset, length, excerpt in
                guard let context, let path else { return }
                let job = Unmanaged<SearchJob>.fromOpaque(context).takeUnretainedValue()
                guard job.rows.count < 200 else { return }
                let url = URL(fileURLWithPath: String(cString: path))
                let row = FileRow(url: url, isDirectory: false, locationID: job.locationID, offset: length == 0 ? nil : offset,
                                  matchLength: length, excerpt: excerpt.map { String(cString: $0) } ?? "")
                if job.seen.insert(row.id).inserted {
                    job.rows.append(row)
                    let now = Date()
                    if now.timeIntervalSince(job.lastUpdate) >= 0.1 {
                        job.lastUpdate = now
                        job.progress?(job.rows)
                    }
                }
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

final class DirectoryCursor {
    static let pageSize = 128
    let url: URL
    private let access: ScopedURL
    private let database: URL?
    private var handle: OpaquePointer?

    init(url: URL, access: ScopedURL, database: URL?) {
        self.url = url
        self.access = access
        self.database = database
    }
    deinit { if let handle { slate_directory_close(handle) } }

    private func opened() throws -> OpaquePointer {
        if let handle { return handle }
        let result = try coordinated(url) { folder in
            slate_directory_open(folder.path, database?.path ?? "")
        }
        guard let result else { throw EditorFailure("Unable to list this folder.") }
        handle = result
        return result
    }

    private final class Batch {
        let url: URL
        var rows: [FileRow] = []
        init(_ url: URL) { self.url = url }
    }
    func page(_ index: Int) throws -> [FileRow] {
        let handle = try opened()
        let batch = Batch(url)
        let count = slate_directory_page(handle, Int64(index * Self.pageSize), Int32(Self.pageSize),
                                         Unmanaged.passUnretained(batch).toOpaque()) { context, name, isDirectory in
            guard let context, let name, let filename = String(validatingUTF8: name) else { return }
            let batch = Unmanaged<Batch>.fromOpaque(context).takeUnretainedValue()
            batch.rows.append(FileRow(url: batch.url.appendingPathComponent(filename, isDirectory: isDirectory != 0),
                                      isDirectory: isDirectory != 0))
        }
        guard count >= 0 else { throw EditorFailure(String(cString: slate_directory_error(handle))) }
        return batch.rows.sorted {
            if $0.isDirectory != $1.isDirectory { return $0.isDirectory }
            return $0.url.lastPathComponent.localizedStandardCompare($1.url.lastPathComponent) == .orderedAscending
        }
    }
    func warm() throws {
        let handle = try opened()
        guard slate_directory_warm(handle, 1024) != 0 else { throw EditorFailure(String(cString: slate_directory_error(handle))) }
    }
    var count: Int? {
        guard let handle else { return nil }
        let n = slate_directory_count(handle)
        return n < 0 ? nil : Int(n)
    }
    var scanned: Int64 { handle.map { slate_directory_scanned($0) } ?? 0 }
    var cached: Bool { handle.map { slate_directory_cached($0) != 0 } ?? false }
}

final class DirectoryBranch: ObservableObject {
    let url: URL
    let access: ScopedURL
    private let database: URL?
    private let queue = DispatchQueue(label: "org.slateeditor.directory", qos: .utility)
    private var cursor: DirectoryCursor
    @Published private(set) var rows: [FileRow] = []
    @Published private(set) var children: [URL: DirectoryBranch] = [:]
    @Published private(set) var pageIndex = 0
    @Published private(set) var count: Int?
    @Published private(set) var busy = false
    @Published private(set) var error: String?
    @Published private var pinned: FileRow?
    private var warming = false
    var more: Bool { count.map { (pageIndex + 1) * DirectoryCursor.pageSize < $0 } ?? (rows.count == DirectoryCursor.pageSize) }
    var displayedRows: [FileRow] {
        guard let pinned else { return rows }
        return [pinned] + rows.filter { $0.url.lastPathComponent != pinned.url.lastPathComponent }
    }

    init(url: URL, access: ScopedURL, database: URL?) {
        self.url = url
        self.access = access
        self.database = database
        cursor = DirectoryCursor(url: url, access: access, database: database)
        load(0)
    }

    func toggle(_ url: URL) {
        if children.removeValue(forKey: url) == nil {
            children[url] = DirectoryBranch(url: url, access: access, database: database)
        }
    }

    // Pin just the known path while its siblings are loaded in bounded pages.
    func reveal(_ components: ArraySlice<String>) {
        guard let name = components.first else { return }
        let folder = components.count > 1
        let child = url.appendingPathComponent(name, isDirectory: folder)
        pinned = FileRow(url: child, isDirectory: folder)
        if folder {
            if children[child] == nil { toggle(child) }
            children[child]?.reveal(components.dropFirst())
        }
    }

    func apply(_ file: URL, exists: Bool) {
        if url.standardizedFileURL.path == file.deletingLastPathComponent().standardizedFileURL.path {
            if exists { pinned = FileRow(url: file, isDirectory: false) }
            else if pinned?.url.lastPathComponent == file.lastPathComponent { pinned = nil }
            reload()
        } else {
            for child in children.values where file.pathComponents.starts(with: child.url.pathComponents) {
                child.apply(file, exists: exists)
            }
        }
    }

    func reload(resetTree: Bool = false) {
        if resetTree {
            pinned = nil
            children.removeAll()
            pageIndex = 0
        }
        cursor = DirectoryCursor(url: url, access: access, database: database)
        warming = false
        busy = false
        count = nil
        error = nil
        let database = self.database, path = url.path
        queue.async { if let database { slate_directory_forget(database.path, path) } }
        load(pageIndex)
    }

    func loadMore() { if more { load(pageIndex + 1) } }
    func previous() { if pageIndex > 0 { load(pageIndex - 1) } }

    private func load(_ index: Int) {
        guard !busy else { return }
        busy = true
        let cursor = self.cursor
        queue.async { [weak self] in
            let result = Result { () -> (Int, [FileRow]) in
                let rows = try cursor.page(index)
                if rows.isEmpty, index > 0, let total = cursor.count {
                    let last = max(0, (total - 1) / DirectoryCursor.pageSize)
                    return (last, try cursor.page(last))
                }
                return (index, rows)
            }
            let total = cursor.count
            DispatchQueue.main.async { [weak self] in
                guard let self, self.cursor === cursor else { return }
                self.busy = false
                do {
                    (self.pageIndex, self.rows) = try result.get()
                    self.count = total
                    if total == nil && !self.warming { self.warm(cursor) }
                } catch { self.error = error.localizedDescription }
            }
        }
    }

    private func warm(_ cursor: DirectoryCursor) {
        warming = true
        queue.async { [weak self] in
            guard self != nil else { return }
            let result = Result { try cursor.warm() }
            let total = cursor.count
            DispatchQueue.main.async { [weak self] in
                guard let self, self.cursor === cursor else { return }
                if case .failure(let error) = result { self.error = error.localizedDescription; self.warming = false }
                else if let total { self.count = total; self.warming = false }
                else { self.warm(cursor) }
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
    @Published private(set) var revealedFile: URL?
    private var revealedLocation: UUID?
    var sidebarLocations: [FolderLocation] {
        guard let root = locations.first(where: { $0.id == revealedLocation }) else { return locations }
        return [root] + locations.filter { $0.id != root.id }
    }
    func clearReveal() {
        revealedLocation = nil
        revealedFile = nil
    }
    @Published var query = "" { didSet { if oldValue != query { search() } } }
    @Published var contents = false { didSet { if oldValue != contents { search() } } }
    @Published private(set) var hits: [FileRow] = []
    @Published private(set) var searching = false
    @Published private(set) var message: String?

    private let database: URL?
    private let defaults: UserDefaults?
    private let savedFolders = "Slate.workspaceFolders"
    private let lastFolder = "Slate.activeWorkspaceFolder"
    private var forgottenGrants: Set<String>
    private let queue = DispatchQueue(label: "org.slateeditor.search", qos: .userInitiated)
    private var searchJob: SearchJob?

    static func applicationWorkspace() -> WorkspaceModel {
        #if os(iOS)
        let local = try? FileManager.default.url(for: .documentDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
        #else
        let local: URL? = nil
        #endif
        let cache = try? FileManager.default.url(for: .cachesDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
            .appendingPathComponent("Slate-directory-names.sqlite")
        return WorkspaceModel(defaults: .standard, documents: local, database: cache)
    }

    init(defaults: UserDefaults? = nil, documents: URL? = nil, database: URL? = nil) {
        self.database = database
        self.defaults = defaults
        forgottenGrants = Set(defaults?.stringArray(forKey: "Slate.forgottenWorkspaceFolders") ?? [])
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
        clearReveal()
        let access = ScopedURL(folder)
        forgottenGrants.remove(folder.standardizedFileURL.path)
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
        guard let grant = file?.parent, !forgottenGrants.contains(grant.url.standardizedFileURL.path),
              !locations.contains(where: { $0.reference.url.standardizedFileURL.path == grant.url.standardizedFileURL.path }) else { return }
        let folder = FolderLocation(id: UUID(), title: grant.url.lastPathComponent, reference: grant.reference, local: false)
        locations.append(folder)
        expand(folder, access: grant)
    }

    // A single-document bookmark never authorizes listing adjacent files.
    // Resolve parent access only through a known folder grant or Slate Documents.
    func showInFolder(_ file: ScopedURL, chooseFolder: @escaping (URL) -> Void) {
        includeGrant(for: file)
        let fileURL = file.url
        let locations = self.locations
        DispatchQueue.global(qos: .userInitiated).async {
            let target = fileURL.resolvingSymlinksInPath().standardizedFileURL
            let components = target.pathComponents
            var found: (UUID, ScopedURL, [String])?
            for location in locations {
                guard let grant = try? location.acquire() else { continue }
                let root = grant.url.resolvingSymlinksInPath().standardizedFileURL.pathComponents
                guard components.starts(with: root), components.count > root.count,
                      components.count - root.count < (found?.2.count ?? Int.max) else { continue }
                let parent = target.deletingLastPathComponent()
                let fd = Darwin.open(parent.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                guard fd >= 0 else { continue }
                close(fd)
                guard FileManager.default.fileExists(atPath: target.path) else { continue }
                found = (location.id, grant, Array(components.dropFirst(root.count)))
            }
            DispatchQueue.main.async {
                if let (id, grant, path) = found, let location = self.locations.first(where: { $0.id == id }) {
                    self.query = ""
                    if self.expanded[id] == nil || self.expanded[id]?.error != nil { self.expand(location, access: grant) }
                    self.expanded[id]?.reveal(path[...])
                    self.revealedLocation = id
                    self.revealedFile = fileURL
                    self.folderPicked += 1
                } else { chooseFolder(fileURL.deletingLastPathComponent()) }
            }
        }
    }

    func toggle(_ location: FolderLocation) {
        if expanded.removeValue(forKey: location.id) == nil { expand(location) }
    }

    private func expand(_ location: FolderLocation, access: ScopedURL? = nil) {
        do {
            let grant = try access ?? location.acquire()
            errors.removeValue(forKey: location.id)
            expanded[location.id] = DirectoryBranch(url: grant.url, access: grant, database: database)
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
        if location.id == revealedLocation { clearReveal() }
        forgottenGrants.insert(location.reference.url.standardizedFileURL.path)
        if let database {
            let path = expanded[location.id]?.url.path ?? location.reference.url.path
            queue.async { slate_directory_forget(database.path, path) }
        }
        expanded.removeValue(forKey: location.id)
        locations.removeAll { $0.id == location.id }
        errors.removeValue(forKey: location.id)
        persist()
        search()
    }

    private func persist() {
        defaults?.set(Array(forgottenGrants), forKey: "Slate.forgottenWorkspaceFolders")
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
        if let database {
            let parent = file.deletingLastPathComponent().path
            queue.async { slate_directory_forget(database.path, parent) }
        }
        for branch in expanded.values { branch.apply(file, exists: exists) }
        search()
    }

    func refresh() {
        for location in locations where expanded[location.id] != nil || errors[location.id] != nil {
            if let branch = expanded[location.id] { branch.reload(resetTree: true) } else { expand(location) }
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
        let locations = self.locations, query = self.query, contents = self.contents, database = self.database
        job.progress = { [weak self, weak job] rows in
            DispatchQueue.main.async {
                guard let self, let job, self.searchJob === job else { return }
                self.hits = rows
            }
        }
        queue.asyncAfter(deadline: .now() + .milliseconds(180)) {
            var errors: Int64 = 0
            for location in locations {
                if job.isCancelled || job.rows.count >= 200 { break }
                do {
                    let result = try job.run(location, database: database, query: query, contents: contents)
                    errors += result < 0 ? 1 : result
                }
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
