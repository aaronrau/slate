#pragma once

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <functional>
#include <memory>
#include <string_view>

namespace slate {

inline constexpr std::size_t max_mermaid_source = 16 * 1024;
bool mermaid_renderer_available();
using MermaidImage = std::shared_ptr<GdkPixbuf>;
enum class MermaidStatus { Ready, Invalid, Unavailable, Cancelled };
struct MermaidResult {
    MermaidImage image;
    MermaidStatus status = MermaidStatus::Unavailable;
};

// Confined to the Markdown worker. A cache hit requires no helper process.
class MermaidRenderer {
public:
    MermaidRenderer();
    ~MermaidRenderer();
    MermaidRenderer(const MermaidRenderer&) = delete;
    MermaidRenderer& operator=(const MermaidRenderer&) = delete;

    MermaidResult render(std::string_view source, const std::function<bool()>& cancelled);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slate
