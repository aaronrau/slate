#include "mermaid_renderer.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
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
        check(renderer.render(flowchart, [] { return true; }).status == MermaidStatus::Cancelled, "cancellation");
        check(renderer.render(std::string(16385, 'x'), running).status == MermaidStatus::Invalid, "source limit");
        if (!mermaid_renderer_available()) {
            check(renderer.render(flowchart, running).status == MermaidStatus::Unavailable, "missing renderer");
            std::cout << "Merman CLI unavailable\n";
            return 77;
        }
        const auto flow = renderer.render(flowchart, running);
        check_diagram(flow);
        const auto sequence = renderer.render("sequenceDiagram\nAlice->>Bob: Hello\nBob-->>Alice: Ready", running);
        check_diagram(sequence);
        check(gdk_pixbuf_get_height(sequence.image.get()) > gdk_pixbuf_get_height(flow.image.get()),
              "sequence diagram reused flowchart crop");
        check(renderer.render("unknown diagram syntax", running).status == MermaidStatus::Invalid, "syntax error");
        const auto cached = renderer.render(flowchart, running);
        check(cached.image == flow.image, "cache should return the original decoded diagram");
    } catch (const std::exception& error) {
        std::cerr << "Mermaid renderer test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Mermaid renderer tests passed\n";
}
