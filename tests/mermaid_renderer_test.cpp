#include "mermaid_renderer.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Color {
    int r, g, b;
    explicit Color(const std::string& hex) {
        auto value = std::stoul(hex.substr(1), nullptr, 16);
        r = (value >> 16) & 255;
        g = (value >> 8) & 255;
        b = value & 255;
    }
    bool matches(const guint8* p) const {
        return std::abs(p[0] - r) <= 3 && std::abs(p[1] - g) <= 3 && std::abs(p[2] - b) <= 3;
    }
    double luminance() const {
        auto linear = [](int c) {
            const auto s = c / 255.0;
            return s <= .04045 ? s / 12.92 : std::pow((s + .055) / 1.055, 2.4);
        };
        return .2126 * linear(r) + .7152 * linear(g) + .0722 * linear(b);
    }
};

double contrast(Color a, Color b) {
    const auto x = a.luminance(), y = b.luminance();
    return (std::max(x, y) + .05) / (std::min(x, y) + .05);
}

void check_diagram(const slate::MermaidResult& result, const slate::Palette& palette) {
    check(result.status == slate::MermaidStatus::Ready && result.image, "diagram did not render");
    auto* image = result.image.get();
    const auto width = gdk_pixbuf_get_width(image);
    const auto height = gdk_pixbuf_get_height(image);
    check(width > 100 && width <= 720 && height > 40 && height <= 540, "unexpected crop");
    const auto* pixels = gdk_pixbuf_read_pixels(image);
    const auto stride = gdk_pixbuf_get_rowstride(image);
    const auto channels = gdk_pixbuf_get_n_channels(image);
    check(gdk_pixbuf_get_has_alpha(image) && channels == 4, "diagram canvas lost its alpha channel");
    const Color label(palette.text), line(palette.muted), node(palette.raised), border(palette.accent);
    unsigned transparent = 0, labels = 0, connectors = 0, fills = 0, outlines = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto* p = pixels + y * stride + x * channels;
            if (p[3] == 0) ++transparent;
            if (p[3] >= 250 && label.matches(p)) ++labels;
            if (p[3] >= 64 && line.matches(p)) ++connectors;
            if (p[3] >= 250 && node.matches(p)) ++fills;
            if (p[3] >= 64 && border.matches(p)) ++outlines;
        }
    }
    check(pixels[3] == 0 && transparent > unsigned(width * height / 10), "opaque diagram canvas");
    check(labels > 12 && fills > 200, "missing themed labels or node fills");
    check(connectors > 5 && outlines > 15, "missing themed arrows or node outlines");
    check(contrast(label, node) >= 4.5 && contrast(line, Color(palette.background)) >= 3,
          "diagram foreground contrast");
}
} // namespace

int main() {
    try {
        using namespace slate;
        const auto running = [] { return false; };
        constexpr auto flowchart = "flowchart LR\nA[Source] -->|label| B[Preview]";
        constexpr auto sequence = "sequenceDiagram\nAlice->>Bob: Hello\nBob-->>Alice: Ready";
        auto dark = default_palette(true);
        MermaidRenderer renderer;
        check(renderer.render(flowchart, dark, [] { return true; }).status == MermaidStatus::Cancelled, "cancellation");
        check(renderer.render(std::string(16385, 'x'), dark, running).status == MermaidStatus::Invalid, "source limit");
        if (!mermaid_renderer_available()) {
            check(renderer.render(flowchart, dark, running).status == MermaidStatus::Unavailable, "missing renderer");
            std::cout << "Merman CLI unavailable\n";
            return 77;
        }
        for (const auto& palette : {dark, default_palette(false)}) {
            const auto flow = renderer.render(flowchart, palette, running);
            check_diagram(flow, palette);
            const auto seq = renderer.render(sequence, palette, running);
            check_diagram(seq, palette);
            check(gdk_pixbuf_get_height(seq.image.get()) > gdk_pixbuf_get_height(flow.image.get()),
                  "sequence diagram reused flowchart crop");
            check(renderer.render(flowchart, palette, running).image == flow.image, "cache missed identical palette");
            auto code_change = palette;
            code_change.code_text = "#ffaa33";
            check(renderer.render(flowchart, code_change, running).image == flow.image, "code color evicted diagram");
        }
        dark.text = "#fff2d8";
        dark.muted = "#a4c9df";
        dark.accent = "#babbf2";
        check_diagram(renderer.render(flowchart, dark, running), dark);
        check(renderer.render("unknown diagram syntax", dark, running).status == MermaidStatus::Invalid, "syntax error");
    } catch (const std::exception& error) {
        std::cerr << "Mermaid renderer test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Mermaid renderer tests passed\n";
}
