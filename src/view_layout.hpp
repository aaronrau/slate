#pragma once

#include "view_rows.hpp"
#include <pango/pangocairo.h>
#include <memory>
#include <vector>

namespace slate::view {

inline void foreground(PangoAttrList* attrs, std::size_t first, std::size_t end, unsigned rgb) {
    auto attr = pango_attr_foreground_new(((rgb >> 16) & 255) * 257,
                                          ((rgb >> 8) & 255) * 257, (rgb & 255) * 257);
    attr->start_index = first;
    attr->end_index = end;
    pango_attr_list_change(attrs, attr);
}

inline void style_markdown(PangoLayout* layout, std::string_view line) {
    auto first = line.find_first_not_of(" \t");
    if (first == line.npos) return;
    auto attrs = pango_attr_list_new();
    auto tail = line.substr(first);
    if (tail.front() == '#') {
        auto n = std::min(tail.size(), tail.find_first_not_of('#'));
        if (n > 0 && n <= 6 && (n == tail.size() || tail[n] == ' ' || tail[n] == '\t')) {
            foreground(attrs, first, line.size(), 0x8fdac9);
            auto weight = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
            weight->start_index = first;
            weight->end_index = line.size();
            pango_attr_list_insert(attrs, weight);
        }
    } else if (tail.front() == '>') {
        foreground(attrs, first, line.size(), 0x93a6aa);
    } else if (tail.starts_with("- ") || tail.starts_with("* ") || tail.starts_with("+ ")) {
        foreground(attrs, first, first + 1, 0xe4b782);
    }
    for (std::size_t i = first; i < line.size();) {
        if (line[i] == '`') {
            auto start = i;
            while (i < line.size() && line[i] == '`') ++i;
            auto delimiter = line.substr(start, i - start);
            auto end = line.find(delimiter, i);
            if (end != line.npos) i = end + delimiter.size();
            foreground(attrs, start, i, 0xe1ba8a);
        } else if (line[i] == '[') {
            auto close = line.find("](", i + 1);
            if (close == line.npos) { ++i; continue; }
            auto end = line.find(')', close + 2);
            if (end == line.npos) { ++i; continue; }
            foreground(attrs, i, close + 1, 0x93bedf);
            foreground(attrs, close + 1, end + 1, 0xa3a4c7);
            i = end + 1;
        } else if (line.substr(i).starts_with("**")) {
            auto end = line.find("**", i + 2);
            if (end == line.npos) { i += 2; continue; }
            foreground(attrs, i, end + 2, 0xe1c7a5);
            i = end + 2;
        } else {
            ++i;
        }
    }
    pango_layout_set_attributes(layout, attrs);
    pango_attr_list_unref(attrs);
}

struct Layout {
    struct Boundary { int raw, display; };
    Row row;
    std::string source;
    std::unique_ptr<PangoLayout, decltype(&g_object_unref)> pango;
    std::vector<Boundary> boundaries;
    int width = 0;

    Layout(Row row, std::string_view raw, PangoContext* context, PangoFontDescription* font,
           PangoTabArray* tabs, bool markdown)
        : row(row), source(raw), pango(pango_layout_new(context), g_object_unref) {
        std::string display;
        display.reserve(raw.size());
        for (std::size_t i = 0; i < raw.size();) {
            boundaries.push_back({static_cast<int>(i), static_cast<int>(display.size())});
            auto tail = raw.substr(i);
            auto length = Rows::valid_length(tail);
            auto byte = static_cast<unsigned char>(tail.front());
            if (length == 1 && (byte >= 128 || (byte < 32 && byte != '\t') || byte == 127))
                display += "�";
            else
                display.append(tail.substr(0, length));
            i += length;
        }
        boundaries.push_back({static_cast<int>(raw.size()), static_cast<int>(display.size())});
        pango_layout_set_font_description(pango.get(), font);
        pango_layout_set_tabs(pango.get(), tabs);
        pango_layout_set_single_paragraph_mode(pango.get(), TRUE);
        pango_layout_set_auto_dir(pango.get(), FALSE);
        pango_layout_set_text(pango.get(), display.c_str(), display.size());
        if (markdown) style_markdown(pango.get(), display);
        pango_layout_get_pixel_size(pango.get(), &width, nullptr);
    }

    int display_index(Pos pos) const {
        auto index = static_cast<int>(std::clamp(pos, row.start, row.end) - row.start);
        auto it = std::upper_bound(boundaries.begin(), boundaries.end(), index,
                                   [](int n, Boundary b) { return n < b.raw; });
        return std::prev(it)->display;
    }
    int x(Pos pos) const {
        PangoRectangle rect{};
        pango_layout_get_cursor_pos(pango.get(), display_index(pos), &rect, nullptr);
        return PANGO_PIXELS(rect.x);
    }
    Pos hit(double x) const {
        int index = 0, trailing = 0;
        pango_layout_xy_to_index(pango.get(), static_cast<int>(x * PANGO_SCALE), 0,
                                 &index, &trailing);
        const char* text = pango_layout_get_text(pango.get());
        index = static_cast<int>(g_utf8_offset_to_pointer(text + index, trailing) - text);
        auto it = std::lower_bound(boundaries.begin(), boundaries.end(), index,
                                   [](Boundary b, int n) { return b.display < n; });
        return row.start + it->raw;
    }
};

} // namespace slate::view
