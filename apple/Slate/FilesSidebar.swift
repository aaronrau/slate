import SwiftUI

// Rows sized for the pointer on the Mac and for touch elsewhere.
#if os(macOS)
private let treeRowHeight: CGFloat = 24
private let hitPadding = EdgeInsets(top: 5, leading: 12, bottom: 5, trailing: 10)
private let hitSpacing: CGFloat = 2
#else
private let treeRowHeight: CGFloat = 37
private let hitPadding = EdgeInsets(top: 10, leading: 10, bottom: 10, trailing: 10)
private let hitSpacing: CGFloat = 3
#endif

// The leading inset of a file row, so its icon lines up with a folder's icon at the same depth.
private func fileIndent(_ depth: Int) -> CGFloat { 28 + CGFloat(min(depth, 6)) * 12 }

struct FilesSidebar: View {
    @ObservedObject var workspace: WorkspaceModel
    @ObservedObject var model: EditorModel
    @ObservedObject var appearance: Appearance
    let pickFolder: () -> Void
    let showInFolder: (ScopedURL) -> Void
    let select: (FileRow, ScopedURL) -> Void
    let selectTab: (NavigationHistory.Tab) -> Void
    let created: (URL) -> Void
    @State private var creation: NewFileRequest?
    @State private var submitted: (String, FileDestination)?

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            // On the Mac these commands live in the File menu and the folders' context menus.
            #if os(iOS)
            HStack {
                Button { newFile(in: nil) } label: {
                    Label("New File…", systemImage: "doc.badge.plus").font(.callout)
                }
                .buttonStyle(.plain).disabled(workspace.locations.isEmpty || model.busy).accessibilityIdentifier("files-new")
                Spacer()
                Button(action: pickFolder) {
                    Label("Open Folder…", systemImage: "folder.badge.plus").font(.callout)
                }
                .buttonStyle(.plain).accessibilityIdentifier("files-open-folder")
            }
            .padding(.horizontal, 14).frame(minHeight: 44)
            if let file = model.file, file.parent == nil {
                Button { showInFolder(file) } label: {
                    Label("Show in Folder", systemImage: "folder").font(.callout)
                }.buttonStyle(.plain).padding(.horizontal, 14).frame(minHeight: 36)
            }
            #endif
            if !workspace.locations.isEmpty {
                #if os(macOS)
                // Sized like a native sidebar search field: 13 pt text in a 24 pt rounded box.
                HStack(spacing: 5) {
                    Image(systemName: "magnifyingglass").font(.chromeIcon).foregroundStyle(appearance.color(.muted))
                    TextField("Search files", text: $workspace.query).textFieldStyle(.plain).font(.chromeRow)
                    if !workspace.query.isEmpty {
                        Button { workspace.query = "" } label: { Image(systemName: "xmark.circle.fill").font(.chromeIcon) }
                            .buttonStyle(.plain).foregroundStyle(appearance.color(.muted)).accessibilityLabel("Clear search")
                    }
                }
                .padding(.horizontal, 7).frame(height: 24)
                .background(appearance.color(.background), in: RoundedRectangle(cornerRadius: 6))
                .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(appearance.color(.border), lineWidth: 1))
                .padding(.horizontal, 10).padding(.vertical, 8)
                #else
                HStack(spacing: 7) {
                    Image(systemName: "magnifyingglass").foregroundStyle(appearance.color(.muted))
                    TextField("Search files", text: $workspace.query).textFieldStyle(.plain)
                    if !workspace.query.isEmpty {
                        Button { workspace.query = "" } label: { Image(systemName: "xmark.circle.fill") }
                            .buttonStyle(.plain).accessibilityLabel("Clear search")
                    }
                }
                .padding(.horizontal, 9).frame(minHeight: 34)
                .background(appearance.color(.background), in: RoundedRectangle(cornerRadius: 5))
                .padding(.horizontal, 10).padding(.bottom, 8)
                #endif
                if !workspace.query.isEmpty {
                    Picker("Search mode", selection: $workspace.contents) {
                        Text("File names").tag(false)
                        Text("Contents").tag(true)
                    }.pickerStyle(.segmented).labelsHidden().padding(.horizontal, 10).padding(.bottom, 8)
                }
            }
            Rectangle().fill(appearance.color(.border)).frame(height: 1)
            ScrollViewReader { reader in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    if workspace.query.isEmpty {
                        ForEach(workspace.sidebarLocations) { location in
                            let branch = workspace.expanded[location.id]
                            FolderButton(name: location.title, expanded: branch != nil, appearance: appearance) {
                                workspace.toggle(location)
                            }
                            .contextMenu {
                                Button("New File…") { newFile(in: FileDestination(location: location, url: branch?.url ?? location.reference.url)) }
                                Button("Refresh") { workspace.refresh() }
                                if !location.local {
                                    Button("Remove from Files") { workspace.forget(location) }
                                }
                            }
                            if let branch {
                                BranchRows(branch: branch, appearance: appearance, selectedPath: selected, depth: 1, select: select,
                                           create: { newFile(in: FileDestination(location: location, url: $0)) },
                                           delete: { model.requestDeletion($0) })
                            }
                            if let error = workspace.errors[location.id] { note(error) }
                        }
                        if workspace.locations.isEmpty {
                            note("Open a folder to browse its files.")
                            #if os(macOS)
                            Button("Open Folder…", action: pickFolder).padding(.horizontal, 14).padding(.top, 2)
                                .accessibilityIdentifier("files-open-folder")
                            #endif
                        }
                        OpenedFiles(history: model.history, appearance: appearance, select: selectTab, reveal: { tab in
                            showInFolder(model.history.folderHint(for: tab))
                        }, delete: { tab in
                            do { model.requestDeletion(try model.history.acquire(tab), tabID: tab.id) }
                            catch { model.errorMessage = error.localizedDescription }
                        })
                    } else {
                        ForEach(workspace.hits) { row in
                            Button {
                                do { select(row, try workspace.access(for: row)) }
                                catch { model.errorMessage = error.localizedDescription }
                            } label: {
                                VStack(alignment: .leading, spacing: hitSpacing) {
                                    Text(row.url.lastPathComponent).font(.chromeRow).lineLimit(1)
                                    let root = workspace.locations.first { $0.id == row.locationID }
                                    let path = root.map { String(row.url.path.dropFirst($0.reference.url.path.count + 1)) } ?? row.url.path
                                    Text(path).font(.chromeDetail).foregroundStyle(appearance.color(.muted)).lineLimit(1)
                                    if !row.excerpt.isEmpty { Text(row.excerpt).font(.chromeDetail.monospaced()).lineLimit(2) }
                                }.frame(maxWidth: .infinity, alignment: .leading).padding(hitPadding).contentShape(Rectangle())
                            }.buttonStyle(.plain)
                            .contextMenu {
                                Button("Delete File…", role: .destructive) {
                                    do { model.requestDeletion(ScopedURL(row.url, parent: try workspace.access(for: row))) }
                                    catch { model.errorMessage = error.localizedDescription }
                                }
                            }
                        }
                        if workspace.searching { ProgressView().controlSize(.small).padding(12) }
                        else if workspace.hits.isEmpty { note("No matches") }
                        if let message = workspace.message { note(message) }
                    }
                }.padding(.vertical, 5)
            }
            .onAppear {
                if let file = workspace.revealedFile { reader.scrollTo(file.standardizedFileURL.path, anchor: .center) }
            }
            .onChange(of: workspace.folderPicked) {
                if let file = workspace.revealedFile { reader.scrollTo(file.standardizedFileURL.path, anchor: .center) }
            }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
        .frame(maxHeight: .infinity, alignment: .top)
        .background(appearance.color(.surface))
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("files-sidebar")
        .sheet(item: $creation, onDismiss: {
            guard let (name, destination) = submitted else { return }
            submitted = nil
            model.createFile(named: name, destination: destination, workspace: workspace, created: created)
        }) { request in
            NewFileForm(request: request) { name, destination in
                submitted = (name, destination)
                creation = nil
            }
        }
    }

    private func newFile(in folder: FileDestination?) {
        let choices = workspace.destinations(including: folder)
        let currentParent = model.file?.url.deletingLastPathComponent().standardizedFileURL.path
        if let destination = folder ?? choices.first(where: { $0.url.standardizedFileURL.path == currentParent }) ?? choices.first {
            creation = NewFileRequest(choices: choices, destination: destination)
        }
    }

    private var selected: String? { (workspace.revealedFile ?? model.file?.url)?.standardizedFileURL.path }
    private func note(_ text: String) -> some View {
        Text(text).font(.chromeNote).foregroundStyle(appearance.color(.muted)).padding(.horizontal, 14).padding(.vertical, 7)
    }
}

private struct FolderButton: View {
    let name: String
    let expanded: Bool
    let appearance: Appearance
    var depth = 0
    var symbol: String?
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 6) {
                Image(systemName: expanded ? "chevron.down" : "chevron.right").font(.chromeChevron)
                    .foregroundStyle(appearance.color(.muted)).frame(width: 10)
                Image(systemName: symbol ?? (expanded ? "folder.fill" : "folder")).font(.chromeIcon)
                    .foregroundStyle(appearance.color(.muted)).frame(width: 17)
                Text(name).font(.chromeRow).fontWeight(depth == 0 ? .semibold : .regular).lineLimit(1)
                Spacer(minLength: 0)
            }
            .padding(.leading, 12 + CGFloat(min(depth, 6)) * 12).padding(.trailing, 8)
            .frame(minHeight: treeRowHeight).contentShape(Rectangle())
        }.buttonStyle(.plain).accessibilityLabel(name).accessibilityValue(expanded ? "Expanded" : "Collapsed")
    }
}

private struct BranchRows: View {
    @ObservedObject var branch: DirectoryBranch
    let appearance: Appearance
    let selectedPath: String?
    let depth: Int
    let select: (FileRow, ScopedURL) -> Void
    let create: (URL) -> Void
    let delete: (ScopedURL) -> Void

    var body: some View {
        LazyVStack(alignment: .leading, spacing: 0) {
            ForEach(branch.displayedRows) { row in
                if row.isDirectory {
                    let child = branch.children[row.url]
                    FolderButton(name: row.url.lastPathComponent, expanded: child != nil, appearance: appearance, depth: depth) {
                        branch.toggle(row.url)
                    }
                    .contextMenu { Button("New File…") { create(row.url) }.accessibilityIdentifier("folder-new-file") }
                    if let child {
                        BranchRows(branch: child, appearance: appearance, selectedPath: selectedPath, depth: depth + 1, select: select, create: create, delete: delete)
                    }
                } else {
                    let selected = selectedPath == row.url.standardizedFileURL.path
                    Button { select(row, branch.access) } label: {
                        HStack(spacing: 6) {
                            Image(systemName: "doc.text").font(.chromeIcon).frame(width: 17).foregroundStyle(appearance.color(.muted))
                            Text(row.url.lastPathComponent).font(.chromeRow).lineLimit(1)
                            Spacer(minLength: 0)
                        }
                        .padding(.leading, fileIndent(depth)).padding(.trailing, 8)
                        .frame(minHeight: treeRowHeight).contentShape(Rectangle())
                        .background(selected ? appearance.color(.accent).opacity(0.18) : Color.clear)
                    }
                    .id(row.url.standardizedFileURL.path)
                    .buttonStyle(.plain).accessibilityLabel(row.url.lastPathComponent)
                    .accessibilityIdentifier("tree-file-" + row.url.lastPathComponent)
                    .accessibilityAddTraits(selected ? .isSelected : [])
                    .contextMenu { Button("Delete File…", role: .destructive) { delete(ScopedURL(row.url, parent: branch.access)) } }
                }
            }
            if branch.busy {
                ProgressView().controlSize(.small).padding(.leading, 32)
            } else if branch.pageIndex > 0 || branch.more {
                HStack {
                    Button { branch.previous() } label: { Image(systemName: "chevron.left") }
                        .disabled(branch.pageIndex == 0).accessibilityLabel("Previous files")
                    let first = branch.pageIndex * DirectoryCursor.pageSize + 1
                    Text("\(first)–\(first + branch.rows.count - 1)" + (branch.count.map { " / \($0)" } ?? ""))
                        .font(.chromeDetail.monospacedDigit()).foregroundStyle(appearance.color(.muted))
                    Button { branch.loadMore() } label: { Image(systemName: "chevron.right") }
                        .disabled(!branch.more).accessibilityLabel("Next files")
                }
                #if os(macOS)
                .controlSize(.small).padding(.leading, fileIndent(depth)).frame(minHeight: treeRowHeight + 4)
                #else
                .padding(.leading, 30).frame(minHeight: 40)
                #endif
            }
            if let error = branch.error {
                Text(error).font(.chromeNote).foregroundStyle(appearance.color(.muted)).padding(.horizontal, 20)
            } else if !branch.busy && branch.rows.isEmpty {
                Text("Empty folder").font(.chromeNote).foregroundStyle(appearance.color(.muted))
                    .padding(.leading, fileIndent(depth) + 23).frame(minHeight: treeRowHeight)
            }
        }
    }
}

private struct OpenedFiles: View {
    @ObservedObject var history: NavigationHistory
    let appearance: Appearance
    let select: (NavigationHistory.Tab) -> Void
    let reveal: (NavigationHistory.Tab) -> Void
    let delete: (NavigationHistory.Tab) -> Void
    @State private var expanded = false
    @State private var oldest = Int.max

    var body: some View {
        if history.tabCount > 0 {
            // Laid out like a top-level folder so it lines up with the tree above it.
            FolderButton(name: "Opened files", expanded: expanded, appearance: appearance, symbol: "clock.arrow.circlepath") {
                if !expanded { oldest = Int.max }
                expanded.toggle()
            }
            if expanded {
                let page = history.openedFiles(endingAt: oldest)
                ForEach(page) { tab in
                    HStack(spacing: 4) {
                        Button { select(tab) } label: {
                            HStack(spacing: 6) {
                                Image(systemName: "doc.text").font(.chromeIcon).frame(width: 17).foregroundStyle(appearance.color(.muted))
                                Text(tab.name).font(.chromeRow).lineLimit(1)
                            }
                            .frame(maxWidth: .infinity, alignment: .leading).padding(.leading, fileIndent(1)).frame(minHeight: treeRowHeight)
                            .contentShape(Rectangle())
                        }.buttonStyle(.plain).accessibilityLabel(tab.name).help(tab.path)
                        Button { reveal(tab) } label: {
                            Image(systemName: "folder").font(.chromeIcon).foregroundStyle(appearance.color(.muted))
                                .frame(width: treeRowHeight, height: treeRowHeight).contentShape(Rectangle())
                        }
                        .buttonStyle(.plain).accessibilityLabel("Show \(tab.name) in Folder").help("Show in Folder")
                    }
                    .contextMenu {
                        Button("Show in Folder") { reveal(tab) }
                        Button("Delete File…", role: .destructive) { delete(tab) }
                    }
                }
                let earlier = page.last.flatMap { history.tabPage(starting: $0.id - 1, backward: true, limit: 1).first }
                let later = page.first.flatMap { history.tabPage(starting: $0.id + 1).last }
                if earlier != nil || later != nil {
                    HStack {
                        Button("Older") { if let earlier { oldest = earlier.id } }.disabled(earlier == nil)
                        Button("Newer") { if let later { oldest = later.id } }.disabled(later == nil)
                    }
                    #if os(macOS)
                    .controlSize(.small).padding(.leading, fileIndent(1)).frame(minHeight: treeRowHeight + 4)
                    #else
                    .padding(.leading, 30)
                    #endif
                }
            }
        }
    }
}

private struct NewFileRequest: Identifiable {
    let id = UUID()
    let choices: [FileDestination]
    let destination: FileDestination
}

private struct NewFileForm: View {
    let request: NewFileRequest
    let submit: (String, FileDestination) -> Void
    @State private var name = ""
    @State private var selected: URL
    @FocusState private var editingName: Bool
    @Environment(\.dismiss) private var dismiss

    init(request: NewFileRequest, submit: @escaping (String, FileDestination) -> Void) {
        self.request = request
        self.submit = submit
        _selected = State(initialValue: request.destination.url)
    }

    var body: some View {
        NavigationStack {
            Form {
                TextField("File name", text: $name)
                    .focused($editingName).autocorrectionDisabled().accessibilityIdentifier("new-file-name")
                    #if os(iOS)
                    .textInputAutocapitalization(.never)
                    #endif
                Picker("Folder", selection: $selected) {
                    ForEach(request.choices) { folder in Text(folder.label).tag(folder.url) }
                }
            }.formStyle(.grouped)
            .navigationTitle("New File")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Create") {
                        if let folder = request.choices.first(where: { $0.url == selected }) { submit(name, folder) }
                    }
                    .disabled(name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }
            }
        }
        .onAppear { editingName = true }
        #if os(macOS)
        .frame(minWidth: 360, minHeight: 200)
        #else
        .presentationDetents([.medium, .large])
        #endif
    }
}
