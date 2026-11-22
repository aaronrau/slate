import Foundation

struct EditorFailure: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}

// A worker owns its own snapshot. No handle is mutated from two threads.
final class Document {
    private let handle: OpaquePointer
    static let pageSize = 64 * 1024

    init() {
        guard let handle = slate_document_new() else { fatalError("Unable to allocate a document") }
        self.handle = handle
    }

    private init(handle: OpaquePointer) { self.handle = handle }
    deinit { slate_document_free(handle) }

    var size: UInt64 { slate_document_size(handle) }
    var dirty: Bool { slate_document_dirty(handle) != 0 }
    private var failure: EditorFailure { EditorFailure(String(cString: slate_document_error(handle))) }

    func snapshot() throws -> Document {
        guard let copy = slate_document_snapshot(handle) else { throw failure }
        return Document(handle: copy)
    }

    func open(_ url: URL) throws {
        guard slate_document_open(handle, url.path) != 0 else { throw failure }
    }

    func save(_ url: URL) throws {
        guard slate_document_save(handle, url.path) != 0 else { throw failure }
    }

    func boundary(_ nominal: UInt64) throws -> UInt64 {
        let result = slate_document_boundary(handle, min(nominal, size))
        guard result != UInt64.max else { throw failure }
        return result
    }

    func read(from start: UInt64, length: Int) throws -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: length)
        let count = bytes.withUnsafeMutableBufferPointer {
            slate_document_read(handle, start, $0.baseAddress, $0.count)
        }
        guard count >= 0 else { throw failure }
        return Array(bytes.prefix(Int(count)))
    }

    func page(starting start: UInt64, ending requestedEnd: UInt64? = nil) throws -> TextPage {
        let start = try boundary(start)
        let end = try boundary(requestedEnd ?? min(size, start + UInt64(Self.pageSize)))
        let bytes = try read(from: start, length: Int(end - start))
        let decoded = String(bytes: bytes, encoding: .utf8)
        return TextPage(start: start, count: bytes.count, text: decoded ?? String(decoding: bytes, as: UTF8.self),
                        isUTF8: decoded != nil)
    }

    func replace(at offset: UInt64, count: Int, with bytes: ArraySlice<UInt8>) throws {
        let success = bytes.withUnsafeBufferPointer {
            slate_document_replace(handle, offset, UInt64(count), $0.baseAddress, $0.count)
        }
        guard success != 0 else { throw failure }
    }
}

struct TextPage {
    var start: UInt64 = 0
    var count = 0
    var text = ""
    var isUTF8 = true
    var end: UInt64 { start + UInt64(count) }

    // TextKit supplies UTF-16 and may combine multiple changes for dictation or IME.
    // Apply only the changed UTF-8 bytes, leaving the rest of the piece tree intact.
    mutating func update(_ replacement: String, in document: Document) throws {
        guard isUTF8 else { throw EditorFailure("This page contains invalid UTF-8 and is read-only.") }
        let old = Array(text.utf8)
        let new = Array(replacement.utf8)
        var prefix = 0
        while prefix < min(old.count, new.count), old[prefix] == new[prefix] { prefix += 1 }
        var suffix = 0
        while suffix < min(old.count, new.count) - prefix,
              old[old.count - suffix - 1] == new[new.count - suffix - 1] { suffix += 1 }
        try document.replace(at: start + UInt64(prefix), count: old.count - prefix - suffix,
                             with: new[prefix..<(new.count - suffix)])
        text = replacement
        count = new.count
    }

    func selection(at offset: UInt64, length: Int) -> NSRange {
        guard isUTF8, offset >= start, offset <= end else { return NSRange(location: 0, length: 0) }
        let bytes = text.utf8
        let first = bytes.index(bytes.startIndex, offsetBy: Int(offset - start))
        let last = bytes.index(first, offsetBy: min(length, count - Int(offset - start)))
        return NSRange(location: String(decoding: bytes[..<first], as: UTF8.self).utf16.count,
                       length: String(decoding: bytes[first..<last], as: UTF8.self).utf16.count)
    }
}

final class ScopedURL {
    let url: URL
    private let scoped: Bool
    // A document selected in a workspace inherits the folder's access grant.
    let parent: ScopedURL?
    lazy var reference = FileReference(self)
    init(_ url: URL, parent: ScopedURL? = nil) {
        self.url = url
        self.parent = parent
        scoped = url.startAccessingSecurityScopedResource()
    }
    deinit {
        if scoped { url.stopAccessingSecurityScopedResource() }
    }
}

func coordinated<T>(_ url: URL, writing: Bool = false, _ action: (URL) throws -> T) throws -> T {
    let coordinator = NSFileCoordinator(filePresenter: nil)
    var error: NSError?
    var outcome: Result<T, Error>?
    if writing {
        coordinator.coordinate(writingItemAt: url, options: .forReplacing, error: &error) { item in
            outcome = Result { try action(item) }
        }
    } else {
        coordinator.coordinate(readingItemAt: url, options: [], error: &error) { item in
            outcome = Result { try action(item) }
        }
    }
    if let error { throw error }
    guard let outcome else { throw EditorFailure("The file provider did not return the document.") }
    return try outcome.get()
}

// File-only grants can forbid creating Slate's sibling temporary file. Foundation
// provides a replacement directory on the same volume for sandbox-safe publication.
struct FileRevision: Equatable {
    let inode: UInt64
    let size: UInt64
    let modified: Date
    init(_ url: URL) throws {
        let fields = try FileManager.default.attributesOfItem(atPath: url.path)
        guard let inode = fields[.systemFileNumber] as? NSNumber,
              let size = fields[.size] as? NSNumber,
              let modified = fields[.modificationDate] as? Date else {
            throw EditorFailure("Could not inspect the file revision.")
        }
        self.inode = inode.uint64Value
        self.size = size.uint64Value
        self.modified = modified
    }
}

func saveDocument(_ snapshot: Document, to target: URL, revision: FileRevision?) throws -> FileRevision {
    try coordinated(target, writing: true) { url in
        do {
            try snapshot.save(url)
        } catch let failure as EditorFailure where failure.message.contains("Create temporary save file") {
            let files = FileManager.default
            let exists = files.fileExists(atPath: url.path)
            let before = exists ? try FileRevision(url) : nil
            if let revision, before != revision { throw EditorFailure("The file changed on disk. Reopen it before saving.") }
            let directory = try files.url(for: .itemReplacementDirectory, in: .userDomainMask,
                                          appropriateFor: url, create: true)
            defer { try? files.removeItem(at: directory) }
            let replacement = directory.appendingPathComponent(url.lastPathComponent)
            try snapshot.save(replacement)
            if exists {
                guard try FileRevision(url) == before else { throw EditorFailure("The file changed during save.") }
                _ = try files.replaceItemAt(url, withItemAt: replacement)
            } else {
                try files.moveItem(at: replacement, to: url)
            }
            try snapshot.open(url)
        }
        return try FileRevision(url)
    }
}
