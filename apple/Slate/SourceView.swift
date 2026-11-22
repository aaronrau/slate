import SwiftUI

private let editLimit = 4 * 1024 * 1024

#if os(macOS)
import AppKit

struct SourceView: NSViewRepresentable {
    @ObservedObject var model: EditorModel
    @ObservedObject var appearance: Appearance
    var textSize: Double = defaultTextSize
    func makeCoordinator() -> Coordinator { Coordinator(model) }

    func makeNSView(context: Context) -> NSScrollView {
        let scroll = NSTextView.scrollableTextView()
        let view = scroll.documentView as! NSTextView
        view.isRichText = false
        view.importsGraphics = false
        view.allowsUndo = true
        view.isAutomaticQuoteSubstitutionEnabled = false
        view.isAutomaticDashSubstitutionEnabled = false
        view.isAutomaticTextReplacementEnabled = false
        view.isAutomaticSpellingCorrectionEnabled = false
        view.isContinuousSpellCheckingEnabled = false
        view.font = .monospacedSystemFont(ofSize: textSize, weight: .regular)
        // With the 5 pt line fragment padding the text starts 16 pt in and 13 pt down, where the
        // preview's text starts at the default 13 pt size, so switching views does not shift it.
        view.textContainerInset = NSSize(width: 11, height: 13)
        view.textContainer?.widthTracksTextView = true
        view.setAccessibilityLabel("Source editor")
        view.delegate = context.coordinator
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        let view = scroll.documentView as! NSTextView
        let color = NSColor(appearance.color(.background))
        view.backgroundColor = color
        scroll.backgroundColor = color
        view.textColor = NSColor(appearance.color(.text))
        view.insertionPointColor = NSColor(appearance.color(.accent))
        if view.font?.pointSize != CGFloat(textSize) { view.font = .monospacedSystemFont(ofSize: textSize, weight: .regular) }
        view.isEditable = model.page.isUTF8 && !model.busy
        let coordinator = context.coordinator
        guard coordinator.revision != model.pageRevision else { return }
        coordinator.loading = true
        view.string = model.page.text
        view.undoManager?.removeAllActions()
        view.setSelectedRange(model.selection)
        view.scrollRangeToVisible(model.selection)
        coordinator.revision = model.pageRevision
        coordinator.loading = false
    }

    final class Coordinator: NSObject, NSTextViewDelegate {
        let model: EditorModel
        var revision = -1
        var loading = false
        init(_ model: EditorModel) { self.model = model }

        func textDidChange(_ notification: Notification) {
            guard !loading, let view = notification.object as? NSTextView else { return }
            model.edit(view.string, caret: view.selectedRange(), composing: view.hasMarkedText())
        }

        func textViewDidChangeSelection(_ notification: Notification) {
            guard !loading, let view = notification.object as? NSTextView else { return }
            model.selection = view.selectedRange()
        }

        func textView(_ textView: NSTextView, shouldChangeTextIn affectedCharRange: NSRange,
                      replacementString: String?) -> Bool {
            guard (replacementString?.utf8.count ?? 0) <= editLimit else {
                model.errorMessage = "Paste up to 4 MiB at a time."
                return false
            }
            return true
        }
    }
}
#else
import UIKit

struct SourceView: UIViewRepresentable {
    @ObservedObject var model: EditorModel
    @ObservedObject var appearance: Appearance
    func makeCoordinator() -> Coordinator { Coordinator(model) }

    func makeUIView(context: Context) -> UITextView {
        let view = UITextView()
        view.font = .monospacedSystemFont(ofSize: 15, weight: .regular)
        view.adjustsFontForContentSizeCategory = true
        view.autocorrectionType = .no
        view.autocapitalizationType = .none
        view.smartDashesType = .no
        view.smartQuotesType = .no
        view.smartInsertDeleteType = .no
        view.spellCheckingType = .no
        view.alwaysBounceVertical = true
        view.keyboardDismissMode = .interactive
        view.accessibilityLabel = "Source editor"
        view.textContainerInset = UIEdgeInsets(top: 14, left: 10, bottom: 18, right: 10)
        view.delegate = context.coordinator
        return view
    }

    func updateUIView(_ view: UITextView, context: Context) {
        view.textColor = UIColor(appearance.color(.text))
        view.backgroundColor = UIColor(appearance.color(.background))
        view.tintColor = UIColor(appearance.color(.accent))
        view.isEditable = model.page.isUTF8 && !model.busy
        let coordinator = context.coordinator
        guard coordinator.revision != model.pageRevision else { return }
        coordinator.loading = true
        view.text = model.page.text
        view.undoManager?.removeAllActions()
        view.selectedRange = model.selection
        view.scrollRangeToVisible(model.selection)
        coordinator.revision = model.pageRevision
        coordinator.loading = false
    }

    final class Coordinator: NSObject, UITextViewDelegate {
        let model: EditorModel
        var revision = -1
        var loading = false
        init(_ model: EditorModel) { self.model = model }

        func textViewDidChange(_ textView: UITextView) {
            guard !loading else { return }
            model.edit(textView.text, caret: textView.selectedRange, composing: textView.markedTextRange != nil)
        }

        func textViewDidChangeSelection(_ view: UITextView) {
            if !loading { model.selection = view.selectedRange }
        }

        func textView(_ textView: UITextView, shouldChangeTextIn range: NSRange, replacementText text: String) -> Bool {
            guard text.utf8.count <= editLimit else {
                model.errorMessage = "Paste up to 4 MiB at a time."
                return false
            }
            return true
        }
    }
}
#endif
