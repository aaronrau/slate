import Foundation
import Combine

final class EditorModel: ObservableObject {
    @Published private(set) var page = TextPage()
    @Published private(set) var size: UInt64 = 0
    @Published private(set) var dirty = false
    @Published private(set) var file: ScopedURL?
    @Published var preview = false
    @Published private(set) var busy = false
    @Published var errorMessage: String?
    @Published var confirmDiscard = false
    @Published var deletion: FileDeletion?
    @Published var needsSaveAs = false
    @Published var exportFile: ExportFile?
    @Published private(set) var pageRevision = 0
    let history: NavigationHistory
    private var attemptedRestore = false
    private var pendingOpen: ScopedURL?
    private var document = Document()
    private var revision: FileRevision?
    private var discardAction: (() -> Void)?
    var selection = NSRange(location: 0, length: 0)
    var title: String { file?.url.lastPathComponent ?? "Untitled" }
    var markdown: Bool { ["md", "markdown", "mdown", "mkd"].contains(file?.url.pathExtension.lowercased() ?? "") }
    var hasPages: Bool { page.start > 0 || page.end < size }

    init(history: NavigationHistory = NavigationHistory(store: .applicationStore())) {
        self.history = history
    }

    func protectChanges(_ action: @escaping () -> Void) {
        guard !busy else { return }
        if dirty {
            discardAction = action
            confirmDiscard = true
        } else { action() }
    }

    func cancelDiscard() {
        discardAction = nil
        confirmDiscard = false
    }

    func discard() {
        let action = discardAction
        discardAction = nil
        action?()
    }

    func rememberPosition() {
        guard file != nil else { return }
        let prefix = (page.text as NSString).substring(to: min(selection.location, page.text.utf16.count))
        history.remember(caret: page.start + UInt64(prefix.utf8.count), preview: preview)
    }

    #if os(macOS)
    func closedWindow() {
        rememberPosition()
        install(Document(), file: nil, revision: nil, page: TextPage())
        attemptedRestore = false
    }
    #endif

    func restore() {
        guard !attemptedRestore else { return }
        attemptedRestore = true
        if !busy, file == nil, let visit = history.currentVisit { navigate(visit: visit) }
    }

    func newDocument() {
        protectChanges { [self] in
            rememberPosition()
            install(Document(), file: nil, revision: nil, page: TextPage())
            history.leaveFile()
        }
    }

    func createFile(named name: String, destination: FileDestination, workspace: WorkspaceModel,
                    created: @escaping (URL) -> Void) {
        protectChanges { [self] in
            busy = true
            DispatchQueue.global(qos: .userInitiated).async {
                let result = Result {
                    let grant = try destination.location.acquire()
                    return try createEmptyFile(named: name, in: destination.url, grant: grant)
                }
                DispatchQueue.main.async {
                    self.busy = false
                    do {
                        let file = try result.get()
                        workspace.applyChange(to: file.url, exists: true, destination: destination)
                        created(file.url)
                        self.load(file, previewMode: false)
                    } catch { self.errorMessage = error.localizedDescription }
                }
            }
        }
    }

    func requestDeletion(_ access: ScopedURL, tabID: Int? = nil) {
        guard !busy else { return }
        busy = true
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result { try FileDeletion(access, tabID: tabID) }
            DispatchQueue.main.async {
                self.busy = false
                do { self.deletion = try result.get() }
                catch { self.errorMessage = error.localizedDescription }
            }
        }
    }

    func confirmDeletion(_ candidate: FileDeletion, workspace: WorkspaceModel) {
        guard !busy else { return }
        deletion = nil
        busy = true
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result { try candidate.remove() }
            DispatchQueue.main.async {
                self.busy = false
                do {
                    try result.get()
                    let url = candidate.access.url
                    let current = self.file?.url.standardizedFileURL.path == url.standardizedFileURL.path
                    if current {
                        self.install(Document(), file: nil, revision: nil, page: TextPage())
                        self.history.leaveFile()
                    }
                    workspace.applyChange(to: url, exists: false)
                    try self.history.remove(candidate)
                } catch { self.errorMessage = error.localizedDescription }
            }
        }
    }

    func openExternal(_ url: URL) {
        attemptedRestore = true
        if busy {
            pendingOpen = ScopedURL(url)
        } else { open(url) }
    }

    func resumePendingOpen() {
        guard !busy, let pendingOpen else { return }
        self.pendingOpen = nil
        protectChanges { [self] in load(pendingOpen) }
    }

    func open(_ url: URL, workspace: ScopedURL? = nil, offset: UInt64? = nil, length: Int = 0) {
        let access = ScopedURL(url, parent: workspace)
        // Search within the live document retains current edits. Disk offsets only
        // refer to the saved revision, so a dirty file goes through the normal prompt.
        if !dirty, let file, file.url.standardizedFileURL == url.standardizedFileURL, let offset {
            jump(to: offset, length: length)
            preview = false
            return
        }
        protectChanges { [self] in load(access, offset: offset, length: length) }
    }

    func navigate(visit: Int) {
        protectChanges { [self] in reopen(visit: visit, backward: visit <= history.position) }
    }

    func showTab(_ tab: NavigationHistory.Tab) {
        guard history.active != tab.id else { return }
        protectChanges { [self] in
            // The strip only retains label metadata. Read the latest saved caret
            // when opening a tab, including after a background transition.
            if let tab = history.tab(id: tab.id) { reopen(tab: tab, visit: nil, backward: true) }
        }
    }

    private func reopen(visit: Int, backward: Bool) {
        if let tab = history.tab(atVisit: visit) { reopen(tab: tab, visit: visit, backward: backward) }
    }

    private func reopen(tab: NavigationHistory.Tab, visit: Int?, backward: Bool) {
        let onFailure: (Error) -> Void = { [self] error in
            history.markUnavailable(tab.id)
            if let visit, let next = history.nextAvailable(after: visit, backward: backward) {
                DispatchQueue.main.async { self.reopen(visit: next, backward: backward) }
            } else {
                if file == nil { history.leaveFile() }
                errorMessage = "Could not reopen “\(tab.name)”. \(error.localizedDescription)"
            }
        }
        do {
            let access = try history.acquire(tab)
            load(access, offset: tab.caret, visit: visit, previewMode: tab.preview, onFailure: onFailure)
        } catch { onFailure(error) }
    }

    private func load(_ access: ScopedURL, offset: UInt64? = nil, length: Int = 0, visit: Int? = nil,
                      previewMode: Bool? = nil, onFailure: ((Error) -> Void)? = nil) {
        rememberPosition()
        busy = true
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result {
                try coordinated(access.url) { url -> (Document, FileRevision, TextPage) in
                    let opened = Document()
                    try opened.open(url)
                    let target = min(offset ?? 0, opened.size)
                    let start = previewMode == nil ? target : target - min(target, UInt64(Document.pageSize / 2))
                    return (opened, try FileRevision(url), try opened.page(starting: start))
                }
            }
            DispatchQueue.main.async {
                self.busy = false
                do {
                    let (opened, rev, page) = try result.get()
                    self.install(opened, file: access, revision: rev, page: page)
                    do { try self.history.record(access, at: visit) }
                    catch { self.errorMessage = error.localizedDescription }
                    if let offset {
                        self.selection = page.selection(at: offset, length: length)
                        self.preview = (previewMode ?? false) && self.markdown && page.isUTF8
                    } else if let previewMode { self.preview = previewMode && self.markdown && page.isUTF8 }
                    self.rememberPosition()
                } catch {
                    if let onFailure { onFailure(error) } else { self.errorMessage = error.localizedDescription }
                }
            }
        }
    }

    private func install(_ opened: Document, file: ScopedURL?, revision: FileRevision?, page: TextPage) {
        self.document = opened
        self.file = file
        self.revision = revision
        self.size = opened.size
        self.dirty = opened.dirty
        self.page = page
        self.preview = markdown && page.isUTF8
        self.selection = NSRange(location: 0, length: 0)
        self.pageRevision += 1
    }

    func edit(_ text: String, caret: NSRange, composing: Bool) {
        guard !busy else { return }
        do {
            var edited = page
            try edited.update(text, in: document)
            size = document.size
            dirty = document.dirty
            page = edited
            selection = caret
            if !composing, edited.count > Document.pageSize * 2 {
                let prefix = (text as NSString).substring(to: min(caret.location, text.utf16.count))
                jump(to: edited.start + UInt64(prefix.utf8.count), centered: true)
            }
        } catch {
            errorMessage = error.localizedDescription
            pageRevision += 1
        }
    }

    func jump(to offset: UInt64, length: Int = 0, centered: Bool = false) {
        guard !busy else { return }
        let target = min(offset, size)
        let preceding = centered ? UInt64(Document.pageSize / 2) : 0
        let start = target - min(target, preceding)
        do {
            page = try document.page(starting: start)
            selection = page.selection(at: target, length: length)
            pageRevision += 1
        } catch { errorMessage = error.localizedDescription }
    }

    func previous() {
        guard !busy, page.start > 0 else { return }
        let end = page.start
        do {
            page = try document.page(starting: end - min(end, UInt64(Document.pageSize)), ending: end)
            selection = NSRange(location: 0, length: 0)
            pageRevision += 1
        } catch { errorMessage = error.localizedDescription }
    }

    func next() { if page.end < size { jump(to: page.end) } }
    func seek(_ fraction: Double) {
        let lastStart = size - min(size, UInt64(Document.pageSize))
        jump(to: min(lastStart, UInt64(Double(lastStart) * min(1, max(0, fraction)))))
    }

    func save() {
        if let file { save(to: file) } else { needsSaveAs = true }
    }

    func save(to target: ScopedURL, onSaved: @escaping () -> Void = {}) {
        guard !busy else { return }
        do {
            let snapshot = try document.snapshot()
            let expected = target.url == file?.url ? revision : nil
            busy = true
            DispatchQueue.global(qos: .userInitiated).async {
                let result = Result { try saveDocument(snapshot, to: target.url, revision: expected) }
                DispatchQueue.main.async {
                    self.busy = false
                    do {
                        self.revision = try result.get()
                        self.document = snapshot
                        self.file = target
                        do { try self.history.record(target) }
                        catch { self.errorMessage = error.localizedDescription }
                        self.dirty = false
                        onSaved()
                    } catch { self.errorMessage = error.localizedDescription }
                }
            }
        } catch { errorMessage = error.localizedDescription }
    }

    func prepareExport(named name: String) {
        guard !busy else { return }
        let name = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty, name != ".", name != "..", !name.contains("/") else {
            errorMessage = "Choose a file name without slashes."
            return
        }
        do {
            let snapshot = try document.snapshot()
            busy = true
            DispatchQueue.global(qos: .userInitiated).async {
                let result = Result { () throws -> ExportFile in
                    let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
                    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: false)
                    let item = ExportFile(url: dir.appendingPathComponent(name))
                    try snapshot.save(item.url)
                    return item
                }
                DispatchQueue.main.async {
                    do {
                        self.exportFile = try result.get()
                        // Keep the source locked until the system has completed or cancelled export.
                    } catch {
                        self.busy = false
                        self.errorMessage = error.localizedDescription
                    }
                }
            }
        } catch { errorMessage = error.localizedDescription }
    }

    func finishExport(_ url: URL?) {
        exportFile = nil
        busy = false
        guard let url else { return }
        load(ScopedURL(url))
    }
}

final class ExportFile: Identifiable {
    let url: URL
    init(url: URL) { self.url = url }
    deinit { try? FileManager.default.removeItem(at: url.deletingLastPathComponent()) }
}
