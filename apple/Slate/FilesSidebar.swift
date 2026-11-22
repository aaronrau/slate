import SwiftUI

struct FilesSidebar: View {
    @ObservedObject var workspace: WorkspaceModel
    @ObservedObject var model: EditorModel
    @ObservedObject var appearance: Appearance
    let pickFolder: () -> Void
    let select: (FileRow, ScopedURL) -> Void
    let selectTab: (NavigationHistory.Tab) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("Files").font(.headline)
                Spacer()
                Button(action: pickFolder) {
                    Label("Open Folder…", systemImage: "folder.badge.plus").font(.callout)
                }
                .buttonStyle(.plain).accessibilityIdentifier("files-open-folder")
            }
            .padding(.horizontal, 14).frame(minHeight: 44)
            if !workspace.locations.isEmpty {
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
                if !workspace.query.isEmpty {
                    Picker("Search mode", selection: $workspace.contents) {
                        Text("File names").tag(false)
                        Text("Contents").tag(true)
                    }.pickerStyle(.segmented).padding(.horizontal, 10).padding(.bottom, 8)
                }
            }
            Rectangle().fill(appearance.color(.border)).frame(height: 1)
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    if workspace.query.isEmpty {
                        ForEach(workspace.locations) { location in
                            let branch = workspace.expanded[location.id]
                            FolderButton(name: location.title, expanded: branch != nil, appearance: appearance) {
                                workspace.toggle(location)
                            }
                            .contextMenu {
                                Button("Refresh") { workspace.refresh() }
                                if !location.local {
                                    Button("Remove from Files") { workspace.forget(location) }
                                }
                            }
                            if let branch {
                                BranchRows(branch: branch, appearance: appearance, selectedPath: selected, depth: 1, select: select)
                            }
                            if let error = workspace.errors[location.id] { note(error) }
                        }
                        if workspace.locations.isEmpty { note("Open a folder to browse its files.") }
                        OpenedFiles(history: model.history, appearance: appearance, select: selectTab)
                    } else {
                        ForEach(workspace.hits) { row in
                            Button {
                                do { select(row, try workspace.access(for: row)) }
                                catch { model.errorMessage = error.localizedDescription }
                            } label: {
                                VStack(alignment: .leading, spacing: 3) {
                                    Text(row.url.lastPathComponent).lineLimit(1)
                                    let root = workspace.locations.first { $0.id == row.locationID }
                                    let path = root.map { String(row.url.path.dropFirst($0.reference.url.path.count + 1)) } ?? row.url.path
                                    Text(path).font(.caption).foregroundStyle(appearance.color(.muted)).lineLimit(1)
                                    if !row.excerpt.isEmpty { Text(row.excerpt).font(.caption.monospaced()).lineLimit(2) }
                                }.frame(maxWidth: .infinity, alignment: .leading).padding(10).contentShape(Rectangle())
                            }.buttonStyle(.plain)
                        }
                        if workspace.searching { ProgressView().controlSize(.small).padding(12) }
                        else if workspace.hits.isEmpty { note("No matches") }
                        if let message = workspace.message { note(message) }
                    }
                }.padding(.vertical, 5)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
        .frame(maxHeight: .infinity, alignment: .top)
        .background(appearance.color(.surface))
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("files-sidebar")
    }

    private var selected: String? { model.file?.url.standardizedFileURL.path }
    private func note(_ text: String) -> some View {
        Text(text).font(.footnote).foregroundStyle(appearance.color(.muted)).padding(.horizontal, 14).padding(.vertical, 7)
    }
}

private struct FolderButton: View {
    let name: String
    let expanded: Bool
    let appearance: Appearance
    var depth = 0
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 6) {
                Image(systemName: expanded ? "chevron.down" : "chevron.right").font(.caption2.weight(.bold)).frame(width: 10)
                Image(systemName: expanded ? "folder.fill" : "folder").foregroundStyle(appearance.color(.muted)).frame(width: 17)
                Text(name).fontWeight(depth == 0 ? .semibold : .regular).lineLimit(1)
                Spacer(minLength: 0)
            }
            .padding(.leading, 12 + CGFloat(min(depth, 6)) * 12).padding(.trailing, 8)
            .frame(minHeight: 37).contentShape(Rectangle())
        }.buttonStyle(.plain).accessibilityLabel(name).accessibilityValue(expanded ? "Expanded" : "Collapsed")
    }
}

private struct BranchRows: View {
    @ObservedObject var branch: DirectoryBranch
    let appearance: Appearance
    let selectedPath: String?
    let depth: Int
    let select: (FileRow, ScopedURL) -> Void

    var body: some View {
        LazyVStack(alignment: .leading, spacing: 0) {
            ForEach(branch.rows) { row in
                if row.isDirectory {
                    let child = branch.children[row.url]
                    FolderButton(name: row.url.lastPathComponent, expanded: child != nil, appearance: appearance, depth: depth) {
                        branch.toggle(row.url)
                    }
                    if let child {
                        BranchRows(branch: child, appearance: appearance, selectedPath: selectedPath, depth: depth + 1, select: select)
                    }
                } else {
                    let selected = selectedPath == row.url.standardizedFileURL.path
                    Button { select(row, branch.access) } label: {
                        HStack(spacing: 6) {
                            Image(systemName: "doc.text").frame(width: 17).foregroundStyle(appearance.color(.muted))
                            Text(row.url.lastPathComponent).lineLimit(1)
                            Spacer(minLength: 0)
                        }
                        .padding(.leading, 28 + CGFloat(min(depth, 6)) * 12).padding(.trailing, 8)
                        .frame(minHeight: 37).contentShape(Rectangle())
                        .background(selected ? appearance.color(.accent).opacity(0.18) : Color.clear)
                    }
                    .buttonStyle(.plain).accessibilityLabel(row.url.lastPathComponent)
                    .accessibilityIdentifier("tree-file-" + row.url.lastPathComponent)
                    .accessibilityAddTraits(selected ? .isSelected : [])
                }
            }
            if branch.busy { ProgressView().controlSize(.small).padding(.leading, 32) }
            else if branch.more { Button("Load more files") { branch.loadMore() }.padding(.leading, 32) }
            else if let error = branch.error {
                Text(error).font(.footnote).foregroundStyle(appearance.color(.muted)).padding(.horizontal, 20)
            } else if branch.rows.isEmpty {
                Text("Empty folder").font(.footnote).foregroundStyle(appearance.color(.muted)).padding(.leading, 40)
            }
        }
    }
}

private struct OpenedFiles: View {
    @ObservedObject var history: NavigationHistory
    let appearance: Appearance
    let select: (NavigationHistory.Tab) -> Void
    @State private var expanded = false
    @State private var start = 0

    var body: some View {
        if history.tabCount > 0 {
            Button {
                if !expanded { start = max(0, history.tabCount - NavigationHistory.stripSize) }
                expanded.toggle()
            } label: {
                Label("Opened files", systemImage: "clock.arrow.circlepath").font(.callout)
                    .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 12).frame(minHeight: 38)
                    .contentShape(Rectangle())
            }.buttonStyle(.plain).accessibilityValue(expanded ? "Expanded" : "Collapsed")
            if expanded {
                ForEach(history.tabPage(starting: start).reversed()) { tab in
                    Button { select(tab) } label: {
                        Label(tab.name, systemImage: "doc.text").lineLimit(1)
                            .frame(maxWidth: .infinity, alignment: .leading).padding(.leading, 30).frame(minHeight: 36)
                            .contentShape(Rectangle())
                    }.buttonStyle(.plain).help(tab.path)
                }
                if start > 0 || start + NavigationHistory.stripSize < history.tabCount {
                    HStack {
                        Button("Older") { start = max(0, start - NavigationHistory.stripSize) }.disabled(start == 0)
                        Button("Newer") { start = min(history.tabCount - 1, start + NavigationHistory.stripSize) }
                            .disabled(start + NavigationHistory.stripSize >= history.tabCount)
                    }.padding(.leading, 30)
                }
            }
        }
    }
}
