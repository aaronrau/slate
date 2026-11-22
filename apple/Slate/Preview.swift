import SwiftUI
import WebKit

struct PreviewContent: Equatable {
    let markdown: String
    let dark: Bool
    let colors: [String: String]
    // The base text size in CSS pixels; nil keeps the stylesheet's 1rem.
    let fontSize: Double?
    init(_ page: TextPage, _ palette: Appearance, fontSize: Double? = nil) {
        markdown = page.text
        dark = palette.dark
        self.fontSize = fontSize
        colors = Dictionary(uniqueKeysWithValues: Ink.allCases.map { ($0.rawValue, palette.hex($0)) })
    }
}

final class PreviewController: NSObject, WKNavigationDelegate {
    private var ready = false
    private var restartAttempted = false
    private var pending: PreviewContent?
    private var displayed: PreviewContent?
    private let entry = Bundle.main.url(forResource: "index", withExtension: "html", subdirectory: "preview")

    func makeView() -> WKWebView {
        let config = WKWebViewConfiguration()
        config.websiteDataStore = .nonPersistent()
        let view = WKWebView(frame: .zero, configuration: config)
        view.navigationDelegate = self
        if let entry {
            view.loadFileURL(entry, allowingReadAccessTo: entry.deletingLastPathComponent())
        } else {
            view.loadHTMLString("<p>Preview resources could not be loaded.</p>", baseURL: nil)
        }
        return view
    }

    func update(_ view: WKWebView, _ content: PreviewContent) {
        pending = content
        render(view)
    }
    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        ready = true
        displayed = nil
        render(webView)
    }
    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        ready = false
        guard !restartAttempted, let entry else { return }
        restartAttempted = true
        webView.loadFileURL(entry, allowingReadAccessTo: entry.deletingLastPathComponent())
    }

    private func render(_ view: WKWebView) {
        guard ready, let pending, pending != displayed else { return }
        displayed = pending
        var palette: [String: Any] = pending.colors
        palette["dark"] = pending.dark
        if let fontSize = pending.fontSize { palette["fontSize"] = fontSize }
        view.callAsyncJavaScript("await window.slatePreview.render(markdown, palette); return true;",
                                 arguments: ["markdown": pending.markdown, "palette": palette],
                                 in: nil, in: .page, completionHandler: nil)
    }

    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        let url = action.request.url
        // Only the bundled renderer and its same-page anchors can be top-level pages.
        let local = url?.isFileURL == true && url?.standardizedFileURL.path == entry?.standardizedFileURL.path
        decisionHandler(local || (entry == nil && url?.absoluteString == "about:blank") ? .allow : .cancel)
    }
}

#if os(macOS)
struct PreviewView: NSViewRepresentable {
    let content: PreviewContent
    func makeCoordinator() -> PreviewController { PreviewController() }
    func makeNSView(context: Context) -> WKWebView { context.coordinator.makeView() }
    func updateNSView(_ view: WKWebView, context: Context) { context.coordinator.update(view, content) }
}
#else
struct PreviewView: UIViewRepresentable {
    let content: PreviewContent
    func makeCoordinator() -> PreviewController { PreviewController() }
    func makeUIView(context: Context) -> WKWebView { context.coordinator.makeView() }
    func updateUIView(_ view: WKWebView, context: Context) { context.coordinator.update(view, content) }
}
#endif
