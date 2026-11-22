import SwiftUI

struct EditorWindow: View {
    @ObservedObject var model: EditorModel
    @ObservedObject var appearance: Appearance
    @ObservedObject var workspace: WorkspaceModel
    let pickFile: () -> Void
    let pickFolder: () -> Void
    let settings: () -> Void
    @State private var sidebar = true
    @State private var compactBrowser = false
    @State private var exportName = ""
    @State private var slider: Double?

    var body: some View {
        GeometryReader { geometry in
            let compact = geometry.size.width < 720
            VStack(spacing: 0) {
                toolbar(compact: compact)
                rule
                HStack(spacing: 0) {
                    if compact ? compactBrowser : sidebar {
                        browser(compact: compact).frame(width: compact ? geometry.size.width : 254)
                        if !compact { Rectangle().fill(appearance.color(.border)).frame(width: 1) }
                    }
                    if !compact || !compactBrowser {
                        VStack(spacing: 0) {
                            TabStrip(history: model.history, model: model, appearance: appearance)
                            rule
                            if model.preview && model.page.isUTF8 {
                                PreviewView(content: PreviewContent(model.page, appearance))
                            } else {
                                SourceView(model: model, appearance: appearance)
                            }
                            rule
                            pageFooter
                        }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                }
            }
            .foregroundStyle(appearance.color(.text))
            .background(appearance.color(.background))
        }
        .onChange(of: workspace.root?.url) { _ in sidebar = true; compactBrowser = true }
        .alert("Unsaved changes to \(model.title)", isPresented: $model.confirmDiscard) {
            Button("Keep Editing", role: .cancel) { model.cancelDiscard() }
            Button("Discard Changes", role: .destructive) { model.discard() }
        }
        .alert("Slate", isPresented: Binding(get: { model.errorMessage != nil },
                                            set: { if !$0 { model.errorMessage = nil } })) {
            Button("OK") { model.errorMessage = nil }
        } message: { Text(model.errorMessage ?? "") }
        #if os(iOS)
        .onChange(of: model.needsSaveAs) { shown in
            if shown { exportName = model.file?.url.lastPathComponent ?? "Untitled.txt" }
        }
        .alert("Save As", isPresented: $model.needsSaveAs) {
            TextField("File name", text: $exportName)
            Button("Cancel", role: .cancel) {}
            Button("Choose Location") { model.prepareExport(named: exportName) }
        }
        #endif
    }

    var rule: some View { Rectangle().fill(appearance.color(.border)).frame(height: 1) }

    private func toolbar(compact: Bool) -> some View {
        HStack(spacing: 12) {
            icon("sidebar.left", "Toggle workspace") {
                if compact { compactBrowser.toggle() } else { sidebar.toggle() }
            }
            if compact && compactBrowser {
                Text("Workspace").fontWeight(.semibold)
            } else if model.markdown {
                Picker("Document view", selection: $model.preview) {
                    Text("Source").tag(false)
                    Text("Preview").tag(true)
                }.pickerStyle(.segmented).frame(maxWidth: 170).disabled(!model.page.isUTF8)
            } else {
                Text("Slate").fontWeight(.semibold)
            }
            Spacer(minLength: 0)
            if model.busy { ProgressView().controlSize(.small) }
            icon("square.and.arrow.down", "Save") { model.save() }.disabled(model.busy)
            Menu {
                Button("New Document", systemImage: "doc.badge.plus") { model.newDocument() }
                Button("Open File…", systemImage: "doc") { pickFile() }
                Button("Open Folder…", systemImage: "folder") { pickFolder() }
                Button("Save As…", systemImage: "square.and.arrow.up") { model.needsSaveAs = true }
                Divider()
                Button("Appearance…", systemImage: "slider.horizontal.3") { settings() }
            } label: { Image(systemName: "ellipsis.circle").frame(minWidth: 30, minHeight: 36) }
            .fixedSize()
            .accessibilityLabel("File and appearance options")
        }
        .padding(.horizontal, 10)
        .frame(height: 46)
        .background(appearance.color(.surface))
    }

    private func icon(_ symbol: String, _ label: String, action: @escaping () -> Void) -> some View {
        Button(action: action) { Image(systemName: symbol).frame(minWidth: 30, minHeight: 36) }
            .buttonStyle(.plain).accessibilityLabel(label).help(label)
    }

    private func browser(compact: Bool) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text(workspace.directory?.lastPathComponent ?? "Workspace")
                    .font(.headline).lineLimit(1)
                Spacer()
                if workspace.canAscend { icon("arrow.up", "Enclosing folder") { workspace.ascend() } }
                icon("folder.badge.plus", "Open folder") { pickFolder() }
            }.padding(.horizontal, 12)
            if workspace.root != nil {
                HStack {
                    Image(systemName: "magnifyingglass")
                    TextField("Search workspace", text: $workspace.query).textFieldStyle(.plain)
                    if !workspace.query.isEmpty { icon("xmark.circle.fill", "Clear search") { workspace.query = "" } }
                }
                .padding(.horizontal, 8).frame(minHeight: 32)
                .background(appearance.color(.background), in: RoundedRectangle(cornerRadius: 5))
                .padding(.horizontal, 10)
                Picker("Search mode", selection: $workspace.contents) {
                    Text("File names").tag(false)
                    Text("Contents").tag(true)
                }.pickerStyle(.segmented).padding(10)
                rule
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 0) {
                        ForEach(workspace.rows) { row in
                            Button {
                                if row.isDirectory { workspace.descend(row.url) }
                                else {
                                    model.open(row.url, workspace: workspace.root, offset: row.offset, length: row.matchLength)
                                    if compact { compactBrowser = false }
                                }
                            } label: {
                                HStack(alignment: .top, spacing: 9) {
                                    Image(systemName: row.isDirectory ? "folder" : "doc.text").frame(width: 16)
                                        .foregroundStyle(appearance.color(.muted))
                                    VStack(alignment: .leading, spacing: 3) {
                                        Text(row.url.lastPathComponent).lineLimit(1)
                                        if !workspace.query.isEmpty, let root = workspace.root?.url {
                                            Text(String(row.url.path.dropFirst(root.path.count + 1)))
                                                .font(.caption).foregroundStyle(appearance.color(.muted)).lineLimit(1)
                                        }
                                        if !row.excerpt.isEmpty { Text(row.excerpt).font(.caption.monospaced()).lineLimit(2) }
                                    }
                                    Spacer(minLength: 0)
                                }
                                .padding(.horizontal, 12).padding(.vertical, 7).contentShape(Rectangle())
                            }.buttonStyle(.plain)
                        }
                        if workspace.more { Button("Load more files") { workspace.loadMore() }.padding(12) }
                        if workspace.busy { ProgressView().padding(12) }
                        if let message = workspace.message { Text(message).font(.caption).padding(12) }
                        else if !workspace.busy && workspace.rows.isEmpty {
                            Text(workspace.query.isEmpty ? "Empty folder" : "No matches").font(.callout).padding(12)
                        }
                    }
                }
            } else {
                Button("Open Folder…", action: pickFolder).padding(12)
            }
            Spacer(minLength: 0)
        }
        .background(appearance.color(.surface))
        .frame(maxHeight: .infinity)
    }

    private var pageFooter: some View {
        VStack(spacing: 3) {
            if !model.page.isUTF8 {
                Text("This page contains invalid UTF-8 and is read-only.").font(.caption)
            }
            HStack(spacing: 8) {
                if model.hasPages {
                    icon("chevron.left", "Previous page") { model.previous() }.disabled(model.page.start == 0)
                    Slider(value: Binding(get: {
                        slider ?? Double(model.page.start) / Double(max(1, model.size - min(model.size, UInt64(Document.pageSize))))
                    }, set: { slider = $0 }), in: 0...1, onEditingChanged: { dragging in
                        if !dragging, let slider { model.seek(slider); self.slider = nil }
                    })
                    .accessibilityLabel("Document position")
                    icon("chevron.right", "Next page") { model.next() }.disabled(model.page.end >= model.size)
                }
                Spacer(minLength: 0)
                Text(model.hasPages ? "\(model.page.start.formatted())–\(model.page.end.formatted()) / \(model.size.formatted()) B"
                                    : "\(model.size.formatted()) bytes")
                    .lineLimit(1).font(.caption.monospacedDigit()).foregroundStyle(appearance.color(.muted))
            }
        }.padding(.horizontal, 10).padding(.vertical, 5).background(appearance.color(.surface))
    }
}

struct TabStrip: View {
    @ObservedObject var history: NavigationHistory
    @ObservedObject var model: EditorModel
    @ObservedObject var appearance: Appearance

    var body: some View {
        HStack(spacing: 0) {
            arrow("chevron.backward", "Go back", history.back)
            arrow("chevron.forward", "Go forward", history.forward)
            if history.hasEarlierTabs { stripButton(forward: false) }
            ScrollViewReader { scroll in
                ScrollView(.horizontal, showsIndicators: false) {
                    LazyHStack(spacing: 0) {
                        if history.active == nil { tab("Untitled", selected: true).id(-1) }
                        ForEach(history.visibleTabs) { item in
                            Button { model.showTab(item) } label: { tab(item.name, selected: history.active == item.id) }
                                .id(item.id).buttonStyle(.plain).help(item.path)
                                .accessibilityAddTraits(history.active == item.id ? .isSelected : [])
                        }
                    }
                }
                .onChange(of: history.active) { id in scroll.scrollTo(id ?? -1, anchor: .center) }
            }
            if history.hasLaterTabs { stripButton(forward: true) }
        }
        .frame(height: 40)
        .background(appearance.color(.surface))
    }

    private func stripButton(forward: Bool) -> some View {
        let label = forward ? "Show next group of tabs" : "Show previous group of tabs"
        return Button { history.shiftStrip(forward: forward) } label: {
            Image(systemName: forward ? "chevron.right.2" : "chevron.left.2").frame(width: 28, height: 38)
        }.buttonStyle(.plain).accessibilityLabel(label).help(label)
    }

    private func arrow(_ symbol: String, _ label: String, _ visit: Int?) -> some View {
        Button { if let visit { model.navigate(visit: visit) } } label: {
            Image(systemName: symbol).frame(width: 31, height: 38)
        }
        .buttonStyle(.plain).disabled(visit == nil || model.busy)
        .accessibilityLabel(label).help(label)
    }

    private func tab(_ name: String, selected: Bool) -> some View {
        HStack(spacing: 5) {
            if selected && model.dirty { Circle().fill(appearance.color(.accent)).frame(width: 6, height: 6).accessibilityLabel("Modified") }
            Text(name).lineLimit(1).font(.callout)
        }
        .padding(.horizontal, 12).frame(height: 40)
        .foregroundStyle(appearance.color(selected ? .text : .muted))
        .background(appearance.color(selected ? .background : .surface))
        .overlay(alignment: .bottom) {
            if selected { Rectangle().fill(appearance.color(.accent)).frame(height: 2) }
        }
        .contentShape(Rectangle())
    }
}
