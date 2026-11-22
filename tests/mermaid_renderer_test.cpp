#include "mermaid_renderer.hpp"

#include <glib.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool has_browser() {
    if (const auto* path = g_getenv("SLATE_CHROME"); path && *path) return access(path, X_OK) == 0;
    for (const auto* name : {"chrome-headless-shell", "chromium", "google-chrome", "chromium-browser"}) {
        auto* path = g_find_program_in_path(name);
        if (path) { g_free(path); return true; }
    }
#ifdef __APPLE__
    return access("/Applications/Chromium.app/Contents/MacOS/Chromium", X_OK) == 0 ||
           access("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", X_OK) == 0;
#else
    return false;
#endif
}

void check_diagram(const slate::MermaidResult& result) {
    check(result.status == slate::MermaidStatus::Ready && result.image, "diagram did not render");
    auto* image = result.image.get();
    const auto width = gdk_pixbuf_get_width(image);
    const auto height = gdk_pixbuf_get_height(image);
    check(width > 100 && width <= 720 && height > 40 && height <= 540, "unexpected crop");
    const auto* pixels = gdk_pixbuf_read_pixels(image);
    const auto stride = gdk_pixbuf_get_rowstride(image);
    const auto channels = gdk_pixbuf_get_n_channels(image);
    unsigned dark_pixels = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto* p = pixels + y * stride + x * channels;
            if (p[0] < 160 && p[1] < 160 && p[2] < 160) ++dark_pixels;
        }
    }
    check(dark_pixels > 100, "diagram labels and graph strokes missing");
}
} // namespace

int main() {
    try {
        using namespace slate;
        const auto running = [] { return false; };
        constexpr auto flowchart = "flowchart LR\nA[Source] --> B[Preview]";
        MermaidRenderer renderer;
        check(!renderer.browser_running(), "eager browser start");
        check(renderer.render(flowchart, [] { return true; }).status == MermaidStatus::Cancelled, "cancellation");
        check(renderer.render(std::string(16385, 'x'), running).status == MermaidStatus::Invalid, "source limit");
        check(!renderer.browser_running(), "invalid or cancelled request started the browser");
        if (!has_browser()) {
            check(renderer.render(flowchart, running).status == MermaidStatus::Unavailable, "missing browser");
            std::cout << "Chrome/Chromium unavailable\n";
            return 77;
        }
        const auto flow = renderer.render(flowchart, running);
        check_diagram(flow);
        const auto sequence = renderer.render("sequenceDiagram\nAlice->>Bob: Hello\nBob-->>Alice: Ready", running);
        check_diagram(sequence);
        check(gdk_pixbuf_get_height(sequence.image.get()) > gdk_pixbuf_get_height(flow.image.get()),
              "sequence diagram reused flowchart crop");
        check(renderer.render("unknown diagram syntax", running).status == MermaidStatus::Invalid, "syntax error");
        renderer.release_browser();
        check(!renderer.browser_running(), "release did not stop browser");
        const auto cached = renderer.render(flowchart, running);
        check(cached.image == flow.image && !renderer.browser_running(), "cached result restarted browser");
    } catch (const std::exception& error) {
        std::cerr << "Mermaid renderer test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Mermaid renderer tests passed\n";
}
