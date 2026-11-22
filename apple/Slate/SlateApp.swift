import SwiftUI
import UniformTypeIdentifiers

enum EditorSheet: Identifiable {
    case appearance, file, folder, export(URL)
    var id: String {
        switch self {
        case .appearance: return "appearance"
        case .file: return "file"
        case .folder: return "folder"
        case .export(let url): return url.absoluteString
        }
    }
}

@main
struct SlateApp: App {
    @StateObject private var model = EditorModel()
    @StateObject private var workspace = WorkspaceModel()
    @StateObject private var appearance = Appearance()
    @State private var sheet: EditorSheet?
    @Environment(\.scenePhase) private var phase

    var body: some Scene {
        WindowGroup {
            EditorWindow(model: model, appearance: appearance, workspace: workspace,
                         pickFile: { pick(folder: false) }, pickFolder: { pick(folder: true) },
                         settings: { sheet = .appearance })
                .preferredColorScheme(appearance.dark ? .dark : .light)
                .tint(appearance.color(.accent))
                .onAppear { model.restore() }
                .onOpenURL { url in
                    if url.hasDirectoryPath { workspace.open(folder: url) }
                    else { model.open(url) }
                }
                .sheet(item: $sheet, onDismiss: {
                    if model.exportFile != nil { model.finishExport(nil) }
                }) { item in
                    switch item {
                    case .appearance:
                        AppearanceView(appearance: appearance)
                            .preferredColorScheme(appearance.dark ? .dark : .light)
                    #if os(iOS)
                    case .file, .folder, .export:
                        DocumentPicker(item: item) { result in
                            self.sheet = nil
                            switch item {
                            case .file: if let result { model.open(result) }
                            case .folder: if let result { workspace.open(folder: result) }
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
            }
            CommandGroup(after: .toolbar) {
                Button("Back") { if let visit = model.history.back { model.navigate(visit: visit) } }
                    .keyboardShortcut("[")
                Button("Forward") { if let visit = model.history.forward { model.navigate(visit: visit) } }
                    .keyboardShortcut("]")
                Button("Toggle Markdown Preview") { model.preview.toggle() }
                    .keyboardShortcut("m", modifiers: [.command, .shift]).disabled(!model.markdown)
            }
            CommandGroup(replacing: .appSettings) {
                Button("Appearance…") { sheet = .appearance }.keyboardShortcut(",")
            }
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
