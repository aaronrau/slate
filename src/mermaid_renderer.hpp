#pragma once

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <functional>
#include <memory>
#include <string_view>

namespace slate {

using MermaidImage = std::shared_ptr<GdkPixbuf>;
enum class MermaidStatus { Ready, Invalid, Unavailable, Cancelled };
struct MermaidResult {
    MermaidImage image;
    MermaidStatus status = MermaidStatus::Unavailable;
};

// Confined to the Markdown diagram worker. Browser creation is lazy and
// release_browser() keeps decoded images available for later snapshots.
class MermaidRenderer {
public:
    MermaidRenderer();
    ~MermaidRenderer();
    MermaidRenderer(const MermaidRenderer&) = delete;
    MermaidRenderer& operator=(const MermaidRenderer&) = delete;

    MermaidResult render(std::string_view source, const std::function<bool()>& cancelled);
    bool browser_running() const;
    void release_browser();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slate
