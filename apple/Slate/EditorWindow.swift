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
    @State private var pendingSelection: URL?
    @State private var exportName = ""
    @State private var slider: Double?

    var body: some View {
        GeometryReader { geometry in
            let compact = geometry.size.width < 720
            VStack(spacing: 0) {
                toolbar(compact: compact)
                rule
                ZStack(alignment: .leading) {
                    HStack(spacing: 0) {
                        if !compact && sidebar {
                            files(compact: false).frame(width: 264)
                            Rectangle().fill(appearance.color(.border)).frame(width: 1)
                        }
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
                    .accessibilityHidden(compact && compactBrowser)
                    if compact && compactBrowser {
                        Button { compactBrowser = false } label: { Color.black.opacity(0.45) }
                            .buttonStyle(.plain).accessibilityLabel("Close Files")
                        files(compact: true)
                            .frame(width: min(322, geometry.size.width * 0.84))
                            .shadow(color: .black.opacity(0.25), radius: 8, x: 3)
                            .transition(.move(edge: .leading))
                    }
                }
            }
            .foregroundStyle(appearance.color(.text))
            .background(appearance.color(.background))
        }
        .onChange(of: workspace.folderPicked) { sidebar = true; compactBrowser = true }
        .onChange(of: model.file?.url) { _, url in
            workspace.includeGrant(for: model.file)
            if let url, pendingSelection?.standardizedFileURL.path == url.standardizedFileURL.path {
                compactBrowser = false
                pendingSelection = nil
            }
        }
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
            Button {
                withAnimation(.easeOut(duration: 0.18)) {
                    if compact { compactBrowser.toggle() } else { sidebar.toggle() }
                }
            } label: {
                Label("Files", systemImage: "line.3.horizontal")
                    .labelStyle(.titleAndIcon).font(.callout.weight(.semibold)).frame(minHeight: 44)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Files")
            .accessibilityValue((compact ? compactBrowser : sidebar) ? "Expanded" : "Collapsed")
            .accessibilityIdentifier("files-toggle")
            if model.markdown {
                Picker("Document view", selection: $model.preview) {
                    Text("Source").tag(false)
                    Text("Preview").tag(true)
                }.pickerStyle(.segmented).frame(maxWidth: 170).disabled(!model.page.isUTF8)
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

    private func files(compact: Bool) -> some View {
        FilesSidebar(workspace: workspace, model: model, appearance: appearance, pickFolder: pickFolder,
                     select: { row, access in
            if compact {
                if model.file?.url.standardizedFileURL.path == row.url.standardizedFileURL.path {
                    compactBrowser = false
                } else { pendingSelection = row.url }
            }
            if row.offset != nil || model.file?.url.standardizedFileURL.path != row.url.standardizedFileURL.path {
                model.open(row.url, workspace: access, offset: row.offset, length: row.matchLength)
            }
        }, selectTab: { tab in
            if compact {
                if model.history.active == tab.id { compactBrowser = false }
                else { pendingSelection = URL(fileURLWithPath: tab.path) }
            }
            model.showTab(tab)
        })
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
