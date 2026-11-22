#pragma once

#include <functional>
#include <memory>
#include <string>

#include <gtk/gtk.h>

namespace slate {

// Colors are opaque #RRGGBB values, usable in GTK CSS, Pango, and SVG.
struct Palette {
    bool dark;
    std::string background, surface, raised, text, muted, border;
    std::string accent, selection, selection_text, code_text, current_line;

    bool operator==(const Palette&) const = default;
};

Palette default_palette(bool dark = true);

class Appearance {
public:
    Appearance();
    ~Appearance();
    const Palette& palette() const;
    void edit(GtkWindow* parent, const std::function<void(const Palette&)>& changed);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slate
