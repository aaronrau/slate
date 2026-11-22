import SwiftUI
import UniformTypeIdentifiers

enum EditorSheet: Identifiable {
    case appearance, file, folder, reveal(URL), export(URL)
    var id: String {
        switch self {
        case .appearance: return "appearance"
        case .file: return "file"
        case .folder: return "folder"
        case .reveal(let file): return "folder-" + file.absoluteString
        case .export(let url): return url.absoluteString
        }
    }
}

@main
struct SlateApp: App {
    #if os(macOS)
    @NSApplicationDelegateAdaptor(SlateAppDelegate.self) private var delegate
    #endif
    @StateObject private var model = EditorModel()
    @StateObject private var workspace = WorkspaceModel.applicationWorkspace()
    @StateObject private var appearance = Appearance()
    @State private var sheet: EditorSheet?
    @Environment(\.scenePhase) private var phase
    #if os(macOS)
    @AppStorage(filesVisibleKey) private var showFiles = true
    #endif

    var body: some Scene {
        WindowGroup {
            EditorWindow(model: model, appearance: appearance, workspace: workspace,
                         pickFile: { pick(folder: false) }, pickFolder: { pick(folder: true) }, showInFolder: reveal,
                         settings: { sheet = .appearance })
                .preferredColorScheme(appearance.dark ? .dark : .light)
                .tint(appearance.color(.accent))
                .onAppear {
                    #if os(macOS)
                    delegate.model = model
                    // Before restore: a file opened from Finder wins over the last visit.
                    delegate.openURL = { [weak model, weak workspace] url in
                        if url.hasDirectoryPath { workspace?.open(folder: url) }
                        else { model?.openExternal(url) }
                    }
                    #endif
                    model.restore()
                }
                .onOpenURL { url in
                    if url.hasDirectoryPath { workspace.open(folder: url) }
                    else { model.openExternal(url) }
                }
                .sheet(item: $sheet, onDismiss: {
                    if model.exportFile != nil { model.finishExport(nil) }
                }) { item in
                    switch item {
                    case .appearance:
                        AppearanceView(appearance: appearance)
                            .preferredColorScheme(appearance.dark ? .dark : .light)
                    #if os(iOS)
                    case .file, .folder, .reveal, .export:
                        DocumentPicker(item: item) { result in
                            self.sheet = nil
                            switch item {
                            case .file: if let result { model.open(result) }
                            case .folder: if let result { workspace.open(folder: result) }
                            case .reveal(let file): if let result { finishReveal(file: file, folder: result) }
                            case .export: model.finishExport(result)
                            case .appearance: break
                            }
                        }
                    #else
                    default: EmptyView()
                    #endif
                    }
                }
                .onChange(of: model.exportFile?.id) { _ in
                    if let item = model.exportFile { sheet = .export(item.url) }
                }
                #if os(macOS)
                .background(WindowCloseGuard(model: model))
                .frame(minWidth: 560, minHeight: 350)
                .onChange(of: model.needsSaveAs) { value in
                    if value {
                        model.needsSaveAs = false
                        let panel = NSSavePanel()
                        panel.nameFieldStringValue = model.file?.url.lastPathComponent ?? "Untitled.txt"
                        panel.canCreateDirectories = true
                        panel.begin { response in
                            if response == .OK, let url = panel.url { model.save(to: ScopedURL(url)) }
                        }
                    }
                }
                #endif
        }
        .onChange(of: phase) { phase in if phase != .active { model.rememberPosition() } }
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("New Document") { model.newDocument() }.keyboardShortcut("n")
                Button("Open File…") { pick(folder: false) }.keyboardShortcut("o")
                Button("Open Folder…") { pick(folder: true) }.keyboardShortcut("o", modifiers: [.command, .shift])
            }
            CommandGroup(replacing: .saveItem) {
                Button("Save") { model.save() }.keyboardShortcut("s").disabled(model.busy)
                Button("Save As…") { model.needsSaveAs = true }.keyboardShortcut("s", modifiers: [.command, .shift])
                    .disabled(model.busy)
                #if os(macOS)
                Divider()
                Button("Show in Files") { if let file = model.file { reveal(file) } }
                    .keyboardShortcut("r", modifiers: [.command, .shift]).disabled(model.file == nil)
                Button("Delete File…") { if let file = model.file { model.requestDeletion(file, tabID: model.history.active) } }
                    .disabled(model.file == nil || model.busy)
                #endif
            }
            CommandGroup(after: .toolbar) {
                #if os(macOS)
                Button(showFiles ? "Hide Files" : "Show Files") { showFiles.toggle() }
                    .keyboardShortcut("s", modifiers: [.command, .control])
                Divider()
                #endif
                Button("Back") { if let visit = model.history.back { model.navigate(visit: visit) } }
                    .keyboardShortcut("[")
                Button("Forward") { if let visit = model.history.forward { model.navigate(visit: visit) } }
                    .keyboardShortcut("]")
                Divider()
                Toggle("Markdown Preview", isOn: $model.preview)
                    .keyboardShortcut("m", modifiers: [.command, .shift]).disabled(!model.markdown)
            }
            CommandGroup(replacing: .appSettings) {
                Button("Appearance…") { sheet = .appearance }.keyboardShortcut(",")
            }
        }
    }

    private func reveal(_ file: ScopedURL) {
        workspace.showInFolder(file) { parent in
            #if os(macOS)
            let panel = NSOpenPanel()
            panel.canChooseDirectories = true
            panel.canChooseFiles = false
            panel.directoryURL = parent
            panel.prompt = "Open Folder"
            panel.message = "Choose the folder containing “\(file.url.lastPathComponent)”."
            panel.begin { response in
                if response == .OK, let url = panel.url { finishReveal(file: file.url, folder: url) }
            }
            #else
            sheet = .reveal(file.url)
            #endif
        }
    }

    private func finishReveal(file: URL, folder: URL) {
        let grant = ScopedURL(folder)
        // The panel's button says Open Folder, so the chosen folder opens either way;
        // the file is selected in it when it lies inside.
        workspace.open(folder: grant.url)
        let root = folder.resolvingSymlinksInPath().standardizedFileURL.pathComponents
        let target = file.resolvingSymlinksInPath().standardizedFileURL
        guard target.pathComponents.starts(with: root), target.pathComponents.count > root.count else { return }
        workspace.showInFolder(ScopedURL(file, parent: grant)) { _ in
            model.errorMessage = "The selected folder could not be read or the file has moved."
        }
    }

    private func pick(folder: Bool) {
        #if os(macOS)
        let panel = NSOpenPanel()
        panel.canChooseDirectories = folder
        panel.canChooseFiles = !folder
        panel.allowsMultipleSelection = false
        panel.begin { response in
            guard response == .OK, let url = panel.url else { return }
            if folder { workspace.open(folder: url) } else { model.open(url) }
        }
        #else
        sheet = folder ? .folder : .file
        #endif
    }
}

#if os(iOS)
import UIKit

struct DocumentPicker: UIViewControllerRepresentable {
    let item: EditorSheet
    let completion: (URL?) -> Void

    func makeCoordinator() -> Coordinator { Coordinator(completion) }
    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        let picker: UIDocumentPickerViewController
        switch item {
        case .folder: picker = UIDocumentPickerViewController(forOpeningContentTypes: [.folder], asCopy: false)
        case .reveal(let file):
            picker = UIDocumentPickerViewController(forOpeningContentTypes: [.folder], asCopy: false)
            picker.directoryURL = file.deletingLastPathComponent()
            picker.title = "Choose the folder containing “\(file.lastPathComponent)”"
        case .export(let url): picker = UIDocumentPickerViewController(forExporting: [url], asCopy: true)
        default: picker = UIDocumentPickerViewController(forOpeningContentTypes: [.data], asCopy: false)
        }
        picker.delegate = context.coordinator
        return picker
    }
    func updateUIViewController(_ view: UIDocumentPickerViewController, context: Context) {}
    final class Coordinator: NSObject, UIDocumentPickerDelegate {
        let completion: (URL?) -> Void
        init(_ completion: @escaping (URL?) -> Void) { self.completion = completion }
        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) { completion(urls.first) }
        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) { completion(nil) }
    }
}
#endif

#if os(macOS)
@MainActor
private func canLeaveEditor(_ model: EditorModel, afterSave: @escaping () -> Void) -> Bool {
    guard !model.busy else { NSSound.beep(); return false }
    model.rememberPosition()
    guard model.dirty else { return true }
    let alert = NSAlert()
    alert.messageText = "Save changes to “\(model.title)”?"
    alert.informativeText = "Closing this document will discard your unsaved changes."
    alert.addButton(withTitle: "Save")
    alert.addButton(withTitle: "Cancel")
    alert.addButton(withTitle: "Discard Changes")
    switch alert.runModal() {
    case .alertFirstButtonReturn:
        if let file = model.file {
            model.save(to: file, onSaved: afterSave)
        } else {
            let panel = NSSavePanel()
            panel.nameFieldStringValue = "Untitled.txt"
            if panel.runModal() == .OK, let url = panel.url {
                model.save(to: ScopedURL(url), onSaved: afterSave)
            }
        }
        return false
    case .alertThirdButtonReturn: return true
    default: return false
    }
}

final class SlateAppDelegate: NSObject, NSApplicationDelegate {
    weak var model: EditorModel?
    // Finder, Open With and `open` deliver files here, not to onOpenURL, and on a
    // cold launch before the window exists: hold the latest until it appears.
    var openURL: ((URL) -> Void)? { didSet { deliver() } }
    private var pending: URL?

    // One editor window: no window tabs (or their View menu items) beside the document tabs.
    func applicationWillFinishLaunching(_ notification: Notification) {
        NSWindow.allowsAutomaticWindowTabbing = false
    }

    func application(_ application: NSApplication, open urls: [URL]) {
        guard let url = urls.last else { return }
        pending = url
        deliver()
        // A launch to open a file skips the untitled window SwiftUI shows at startup,
        // and after the last window closed there is none: ask SwiftUI for one.
        DispatchQueue.main.async {
            if let window = NSApp.windows.first(where: { $0.isVisible && $0.canBecomeMain }) {
                window.makeKeyAndOrderFront(nil)
            } else {
                _ = NSApp.delegate?.applicationOpenUntitledFile?(NSApp)
            }
        }
    }

    private func deliver() {
        guard let openURL, let url = pending else { return }
        pending = nil
        openURL(url)
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let model else { return .terminateNow }
        return canLeaveEditor(model) { sender.terminate(nil) } ? .terminateNow : .terminateCancel
    }
}

// SwiftUI owns the window delegate. Forward all of its selectors while handling
// the one AppKit sends before closing a window via the title bar or Command-W.
struct WindowCloseGuard: NSViewRepresentable {
    let model: EditorModel
    func makeNSView(context: Context) -> Host { Host(model) }
    func updateNSView(_ view: Host, context: Context) {
        view.window?.isDocumentEdited = model.dirty
        view.window?.title = model.title
    }
    final class Host: NSView {
        let observer: Delegate
        init(_ model: EditorModel) {
            observer = Delegate(model)
            super.init(frame: .zero)
        }
        required init?(coder: NSCoder) { nil }
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if let window, window.delegate !== observer {
                observer.previous = window.delegate
                window.delegate = observer
            }
        }
    }

    final class Delegate: NSObject, NSWindowDelegate {
        let model: EditorModel
        weak var previous: NSWindowDelegate?
        init(_ model: EditorModel) { self.model = model }

        override func responds(to selector: Selector!) -> Bool {
            super.responds(to: selector) || previous?.responds(to: selector) == true
        }
        override func forwardingTarget(for selector: Selector!) -> Any? {
            previous?.responds(to: selector) == true ? previous : super.forwardingTarget(for: selector)
        }
        func windowShouldClose(_ sender: NSWindow) -> Bool {
            guard canLeaveEditor(model, afterSave: { [weak sender] in sender?.performClose(nil) }) else { return false }
            return previous?.windowShouldClose?(sender) ?? true
        }
        func windowWillClose(_ notification: Notification) {
            previous?.windowWillClose?(notification)
            model.closedWindow()
        }
    }
}
#endif
