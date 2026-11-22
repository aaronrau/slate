import Foundation
import Combine

struct FileRow: Identifiable {
    let url: URL
    let isDirectory: Bool
    var offset: UInt64? = nil
    var matchLength = 0
    var excerpt = ""
    var id: String { url.path + "\u{0}" + String(offset ?? 0) }
}

private final class SearchJob {
    let handle: OpaquePointer
    var rows: [FileRow] = []
    init?() {
        guard let handle = slate_search_new() else { return nil }
        self.handle = handle
    }
    deinit { slate_search_free(handle) }
    func cancel() { slate_search_cancel(handle) }
    func run(root: URL, query: String, contents: Bool) -> Int64 {
        slate_search_run(handle, root.path, query, contents ? 1 : 0, Unmanaged.passUnretained(self).toOpaque()) {
            context, path, offset, length, excerpt in
            guard let context, let path else { return }
            let job = Unmanaged<SearchJob>.fromOpaque(context).takeUnretainedValue()
            let url = URL(fileURLWithPath: String(cString: path))
            job.rows.append(FileRow(url: url, isDirectory: false, offset: length == 0 ? nil : offset,
                                    matchLength: length, excerpt: excerpt.map { String(cString: $0) } ?? ""))
        }
    }
}

private final class DirectoryCursor {
    let url: URL
    var iterator: FileManager.DirectoryEnumerator?
    var finished = false
    init(url: URL) { self.url = url }

    func next() throws -> [FileRow] {
        guard !finished else { return [] }
        return try coordinated(url) { url in
            let keys: [URLResourceKey] = [.isDirectoryKey, .isRegularFileKey, .isSymbolicLinkKey]
            if iterator == nil {
                iterator = FileManager.default.enumerator(at: url, includingPropertiesForKeys: keys,
                                                         options: [.skipsSubdirectoryDescendants])
            }
            guard let iterator else { throw EditorFailure("Unable to list this folder.") }
            var rows: [FileRow] = []
            while rows.count < 128 {
                guard let entry = iterator.nextObject() as? URL else { finished = true; break }
                guard let info = try? entry.resourceValues(forKeys: Set(keys)), info.isSymbolicLink != true,
                      info.isDirectory == true || info.isRegularFile == true else { continue }
                rows.append(FileRow(url: entry, isDirectory: info.isDirectory == true))
            }
            return rows.sorted {
                if $0.isDirectory != $1.isDirectory { return $0.isDirectory }
                return $0.url.lastPathComponent.localizedStandardCompare($1.url.lastPathComponent) == .orderedAscending
            }
        }
    }
}

final class WorkspaceModel: ObservableObject {
    @Published private(set) var root: ScopedURL?
    @Published private(set) var directory: URL?
    @Published private(set) var rows: [FileRow] = []
    @Published var query = "" { didSet { if oldValue != query { refresh() } } }
    @Published var contents = false { didSet { if oldValue != contents { refresh() } } }
    @Published private(set) var busy = false
    @Published private(set) var more = false
    @Published private(set) var message: String?
    private let queue = DispatchQueue(label: "org.slateeditor.workspace", qos: .userInitiated)
    private var cursor: DirectoryCursor?
    private var search: SearchJob?
    private var generation = 0
    var canAscend: Bool { directory != root?.url }

    func open(folder: URL) {
        root = ScopedURL(folder)
        directory = folder
        query = ""
        refresh()
    }
    func descend(_ url: URL) { directory = url; refresh() }
    func ascend() { if canAscend, let directory { descend(directory.deletingLastPathComponent()) } }

    func refresh() {
        search?.cancel()
        search = nil
        generation += 1
        let ticket = generation
        rows = []
        more = false
        message = nil
        guard let root, let directory else { return }
        if query.isEmpty {
            cursor = DirectoryCursor(url: directory)
            busy = false
            loadMore()
        } else if let job = SearchJob() {
            search = job
            busy = true
            let query = self.query, contents = self.contents
            queue.asyncAfter(deadline: .now() + .milliseconds(180)) {
                let errors = job.run(root: root.url, query: query, contents: contents)
                DispatchQueue.main.async {
                    guard self.generation == ticket else { return }
                    self.rows = job.rows
                    self.busy = false
                    if errors < 0 { self.message = "Search failed." }
                    else if job.rows.count == 200 { self.message = "Showing the first 200 matches." }
                    else if errors > 0 { self.message = "\(errors) unreadable entries" }
                }
            }
        }
    }

    func loadMore() {
        guard !busy, let cursor, let root else { return }
        busy = true
        let ticket = generation
        queue.async {
            let result = withExtendedLifetime(root) { Result { try cursor.next() } }
            let more = !cursor.finished
            DispatchQueue.main.async {
                guard self.generation == ticket else { return }
                self.busy = false
                do {
                    self.rows += try result.get()
                    self.more = more
                } catch { self.message = error.localizedDescription; self.more = false }
            }
        }
    }
}
