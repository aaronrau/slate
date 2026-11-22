#include "editor_view.hpp"
#include "view_layout.hpp"

#include <array>
#include <cmath>
#include <deque>
#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>

namespace slate {

using view::Layout;
using view::Pos;
using view::Row;
using view::Rows;

namespace {
constexpr int inset = 7;
constexpr std::size_t history_limit = 8 * 1024 * 1024;

void color(cairo_t* cr, unsigned rgb) {
    cairo_set_source_rgb(cr, ((rgb >> 16) & 255) / 255., ((rgb >> 8) & 255) / 255.,
                         (rgb & 255) / 255.);
}
void fill(cairo_t* cr, unsigned rgb, double x, double y, double w, double h) {
    color(cr, rgb);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}
std::string hex_address(Pos pos) {
    std::ostringstream stream;
    stream << "0x" << std::hex << pos;
    return stream.str();
}
struct Edit {
    Pos pos, cursor, anchor;
    std::string before, after;
    std::size_t cost() const { return before.size() + after.size() + sizeof(Edit); }
};
} // namespace

struct EditorView::Impl : std::enable_shared_from_this<EditorView::Impl> {
    EditorView& owner;
    Document& doc;
    Rows rows;
    GtkWidget* root = gtk_grid_new();
    GtkWidget* area = gtk_drawing_area_new();
    GtkAdjustment* vertical = gtk_adjustment_new(0, 0, 1, 1, 1, 1);
    GtkAdjustment* horizontal = gtk_adjustment_new(0, 0, 1, 1, 1, 1);
    GtkWidget* vbar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, vertical);
    GtkWidget* hbar = gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, horizontal);
    GtkWidget* context_menu = nullptr;
    std::array<GtkWidget*, 6> menu_items{};
    GtkIMContext* im = gtk_im_multicontext_new();
    PangoFontDescription* font = pango_font_description_from_string("Monospace 11");
    PangoTabArray* tabs = pango_tab_array_new(1, TRUE);

    Pos caret = 0, anchor = 0, top = 0, bottom = 0, generation = 0;
    std::optional<Pos> top_line = 1;
    // A soft wrap shares one byte offset between the end of a display row and
    // the start of the next. Retain the row selected by End, a click or Up/Down.
    std::optional<Pos> caret_display_row;
    int width = 1, height = 1, cell = 9, line_height = 24, font_height = 18, gutter = 56;
    int preferred_x = -1, widest = 0;
    bool needs_layout = true, needs_bottom = true, setting_scroll = false, dragging = false;
    bool markdown = false, crlf = false, draw_failed = false;
    double wheel = 0, drag_x = 0, drag_y = 0;
    guint drag_timer = 0;
    std::vector<Layout> visible;
    std::deque<Edit> past, future;
    std::size_t history_bytes = 0;
    bool consecutive = false;
    std::string preedit;

    Impl(EditorView& owner, Document& doc) : owner(owner), doc(doc), rows(doc) {
        g_object_ref_sink(root);
        g_object_ref_sink(area);
        g_object_ref_sink(vbar);
        g_object_ref_sink(hbar);
        g_object_ref(vertical);
        g_object_ref(horizontal);
        gtk_widget_set_hexpand(area, TRUE);
        gtk_widget_set_vexpand(area, TRUE);
        gtk_widget_set_can_focus(area, TRUE);
        gtk_widget_set_name(area, "editor-canvas");
        atk_object_set_name(gtk_widget_get_accessible(area), "Editor");
        gtk_widget_set_no_show_all(hbar, TRUE);
        gtk_widget_set_no_show_all(vbar, TRUE);
        gtk_grid_attach(GTK_GRID(root), area, 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(root), vbar, 1, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(root), hbar, 0, 1, 1, 1);
        gtk_widget_add_events(area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
            GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK |
            GDK_FOCUS_CHANGE_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
        gtk_widget_show(area);
        connect();
        measure();
    }
    ~Impl() {
        stop_drag();
        if (context_menu) {
            gtk_widget_destroy(context_menu);
            g_object_unref(context_menu);
        }
        for (auto obj : {G_OBJECT(area), G_OBJECT(vertical), G_OBJECT(horizontal), G_OBJECT(im)})
            g_signal_handlers_disconnect_by_data(obj, this);
        gtk_im_context_set_client_window(im, nullptr);
        g_object_unref(im);
        pango_tab_array_free(tabs);
        pango_font_description_free(font);
        for (auto obj : {G_OBJECT(root), G_OBJECT(area), G_OBJECT(hbar), G_OBJECT(vbar),
                         G_OBJECT(vertical), G_OBJECT(horizontal)})
            g_object_unref(obj);
    }

    template<class Fn> gboolean guard(Fn&& fn) {
        try { return fn(); }
        catch (const std::exception& e) {
            if (owner.on_error) owner.on_error(e.what());
            else g_warning("Editor: %s", e.what());
            return TRUE;
        }
    }
    using Self = Impl;
    void connect() {
        g_signal_connect(area, "draw", G_CALLBACK(+[](GtkWidget*, cairo_t* cr, Self* s) -> gboolean {
            if (s->draw_failed) return TRUE;
            return s->guard([&] {
                // Modal error reporting can run a nested GTK draw loop.
                s->draw_failed = true;
                s->draw(cr);
                s->draw_failed = false;
                return TRUE;
            });
        }), this);
        g_signal_connect(area, "size-allocate", G_CALLBACK(+[](GtkWidget*, GtkAllocation* a, Self* s) {
            if (s->width == a->width && s->height == a->height) return;
            s->width = a->width;
            s->height = a->height;
            s->needs_layout = s->needs_bottom = true;
        }), this);
        g_signal_connect(area, "realize", G_CALLBACK(+[](GtkWidget* area, Self* s) {
            auto window = gtk_widget_get_window(area);
            gtk_im_context_set_client_window(s->im, window);
            auto cursor = gdk_cursor_new_from_name(gtk_widget_get_display(area), "text");
            gdk_window_set_cursor(window, cursor);
            if (cursor) g_object_unref(cursor);
            s->measure();
        }), this);
        g_signal_connect(area, "unrealize", G_CALLBACK(+[](GtkWidget*, Self* s) {
            gtk_im_context_set_client_window(s->im, nullptr);
            s->stop_drag();
        }), this);
        g_signal_connect(area, "focus-in-event", G_CALLBACK(+[](GtkWidget* w, GdkEventFocus*, Self* s) -> gboolean {
            gtk_im_context_focus_in(s->im);
            gtk_widget_queue_draw(w);
            return FALSE;
        }), this);
        g_signal_connect(area, "focus-out-event", G_CALLBACK(+[](GtkWidget* w, GdkEventFocus*, Self* s) -> gboolean {
            gtk_im_context_focus_out(s->im);
            s->stop_drag();
            gtk_widget_queue_draw(w);
            return FALSE;
        }), this);
        g_signal_connect(im, "commit", G_CALLBACK(+[](GtkIMContext*, const char* text, Self* s) {
            s->guard([&] { if (s->enabled()) s->insert(text, true); return TRUE; });
        }), this);
        g_signal_connect(im, "preedit-changed", G_CALLBACK(+[](GtkIMContext* im, Self* s) {
            gchar* text = nullptr;
            gtk_im_context_get_preedit_string(im, &text, nullptr, nullptr);
            s->preedit = text ? text : "";
            g_free(text);
            gtk_widget_queue_draw(s->area);
        }), this);
        g_signal_connect(area, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* e, Self* s) {
            return s->guard([&] { return s->key(e); });
        }), this);
        g_signal_connect(area, "key-release-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* e, Self* s) {
            return s->guard([&] { return gtk_im_context_filter_keypress(s->im, e); });
        }), this);
        g_signal_connect(area, "button-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton* e, Self* s) {
            return s->guard([&] { return s->press(e); });
        }), this);
        g_signal_connect(area, "button-release-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton* e, Self* s) -> gboolean {
            if (e->button != 1) return FALSE;
            s->stop_drag();
            return TRUE;
        }), this);
        g_signal_connect(area, "motion-notify-event", G_CALLBACK(+[](GtkWidget*, GdkEventMotion* e, Self* s) {
            return s->guard([&] { return s->motion(e->x, e->y); });
        }), this);
        g_signal_connect(area, "scroll-event", G_CALLBACK(+[](GtkWidget*, GdkEventScroll* e, Self* s) {
            return s->guard([&] { return s->scroll(e); });
        }), this);
        g_signal_connect(vertical, "value-changed", G_CALLBACK(+[](GtkAdjustment* a, Self* s) {
            if (s->setting_scroll) return;
            s->draw_failed = false;
            s->guard([&] {
                s->set_top(s->rows.start(static_cast<Pos>(gtk_adjustment_get_value(a))));
                gtk_widget_queue_draw(s->area);
                return TRUE;
            });
        }), this);
        g_signal_connect(horizontal, "value-changed", G_CALLBACK(+[](GtkAdjustment*, Self* s) {
            if (!s->setting_scroll) gtk_widget_queue_draw(s->area);
        }), this);
    }

    bool enabled() const { return gtk_widget_is_sensitive(area); }
    void measure() {
        visible.clear();
        auto probe = gtk_widget_create_pango_layout(area, "0");
        pango_layout_set_font_description(probe, font);
        pango_layout_get_pixel_size(probe, &cell, &font_height);
        g_object_unref(probe);
        cell = std::max(1, cell);
        line_height = std::max(18, font_height + 5);
        pango_tab_array_set_tab(tabs, 0, PANGO_TAB_LEFT, cell * 4);
        needs_layout = needs_bottom = true;
    }
    int page_rows() const { return std::max(1, (height - inset) / line_height); }
    int text_left() const { return gutter + 15; }
    double scroll_x() const { return gtk_adjustment_get_value(horizontal); }
    void set_top(Pos next) {
        if (next == top) return;
        Pos a = std::min(top, next), b = std::max(top, next);
        if (top_line && b - a <= 65536 && b <= doc.size()) {
            auto span = rows.bytes(a, b - a);
            auto n = std::count(span.begin(), span.end(), '\n');
            *top_line = next >= top ? *top_line + n : *top_line - n;
        } else if (next <= 65536) {
            auto span = rows.bytes(0, next);
            top_line = 1 + std::count(span.begin(), span.end(), '\n');
        } else {
            top_line.reset();
        }
        top = next;
        needs_layout = true;
    }
    Pos step_rows(Pos pos, int steps) {
        for (; steps < 0 && pos; ++steps) pos = rows.start(pos - 1);
        for (; steps > 0 && pos < doc.size(); --steps) {
            auto row = rows.at(pos);
            if (rows.last(row)) break;
            pos = row.next;
        }
        return pos;
    }
    void update_bottom() {
        if (!needs_bottom) return;
        bottom = step_rows(rows.start(doc.size()), 1 - page_rows());
        needs_bottom = false;
    }
    Layout make_layout(Row row) {
        return Layout(row, rows.bytes(row.start, row.end - row.start),
                      gtk_widget_get_pango_context(area), font, tabs, markdown);
    }
    void sync() {
        auto extension = doc.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), g_ascii_tolower);
        bool md = extension == ".md" || extension == ".markdown";
        if (md != markdown) { markdown = md; visible.clear(); needs_layout = true; }
        if (!needs_layout) return;
        update_bottom();
        if (top > bottom) set_top(bottom);
        auto previous = std::move(visible);
        visible.clear();
        visible.reserve(page_rows() + 1);
        auto pos = top;
        std::size_t previous_row = 0;
        for (int i = 0; i <= page_rows(); ++i) {
            auto row = rows.at(pos);
            // Scrolling shifts the screen index. Editing can shift byte offsets.
            // In either case check the bytes before reusing a shaped row.
            while (previous_row < previous.size() && previous[previous_row].row.start < pos)
                ++previous_row;
            auto cached = previous_row < previous.size() && previous[previous_row].row.start == pos
                ? previous_row : static_cast<std::size_t>(i);
            if (cached < previous.size() && previous[cached].pango &&
                rows.bytes(row.start, row.end - row.start) == previous[cached].source) {
                visible.push_back(std::move(previous[cached]));
                visible.back().row = row;
            } else {
                visible.push_back(make_layout(row));
            }
            widest = std::max(widest, visible.back().width);
            if (rows.last(row)) break;
            pos = row.next;
        }
        auto label = top_line ? std::to_string(*top_line + visible.size()) : hex_address(pos);
        gutter = std::max(51, 19 + static_cast<int>(label.size()) * cell);
        double page = std::max(1, width - text_left() - 12);
        auto span = std::max<Pos>(1, visible.back().row.next - top);
        setting_scroll = true;
        gtk_adjustment_configure(vertical, top, 0, static_cast<double>(bottom) + span,
                                 std::max(1., double(span) / visible.size()), span, span);
        gtk_adjustment_configure(horizontal, scroll_x(), 0, std::max<double>(page, widest + 10),
                                 cell * 3, page * .9, page);
        gtk_widget_set_visible(vbar, bottom > 0);
        gtk_widget_set_visible(hbar, widest + 10 > page);
        setting_scroll = false;
        needs_layout = false;
    }
    Pos caret_row() { return caret_display_row ? *caret_display_row : rows.start(caret); }

    void reveal() {
        sync();
        Pos start = caret_row();
        if (start < top) set_top(start);
        else if (start > visible[std::min<int>(page_rows(), visible.size()) - 1].row.start)
            set_top(std::min(bottom, step_rows(start, 1 - page_rows())));
        sync();
        for (const auto& layout : visible) {
            if (layout.row.start != start) continue;
            int x = layout.x(caret);
            double left = scroll_x(), page = gtk_adjustment_get_page_size(horizontal);
            if (x < left) gtk_adjustment_set_value(horizontal, std::max(0, x - cell * 2));
            else if (x >= left + page - cell) gtk_adjustment_set_value(horizontal, x - page + cell * 2);
            break;
        }
    }
    void changed(bool show = true) {
        ++generation;
        draw_failed = false;
        if (show) reveal();
        gtk_widget_queue_draw(area);
        if (owner.on_change) owner.on_change();
    }
    void move(Pos pos, bool extend, bool vertical_move = false, std::optional<Pos> row = {}) {
        caret_display_row = row;
        caret = std::min(pos, doc.size());
        if (!extend) anchor = caret;
        if (!vertical_move) preferred_x = -1;
        consecutive = false;
        changed();
    }
    void reset() {
        gtk_im_context_reset(im);
        stop_drag();
        rows.invalidate();
        caret = anchor = top = 0;
        caret_display_row.reset();
        top_line = 1;
        widest = 0;
        preferred_x = -1;
        past.clear();
        future.clear();
        history_bytes = 0;
        consecutive = false;
        needs_bottom = needs_layout = true;
        auto sample = rows.bytes(0, 8192);
        auto lf = sample.find('\n');
        crlf = lf != sample.npos && lf && sample[lf - 1] == '\r';
        gtk_adjustment_set_value(horizontal, 0);
        changed();
    }
    void draw(cairo_t* cr) {
        sync();
        color(cr, 0x171b20);
        cairo_paint(cr);
        fill(cr, 0x161a1f, 0, 0, gutter, height);
        fill(cr, 0x242a31, gutter, 0, 1, height);
        auto label = gtk_widget_create_pango_layout(area, "");
        pango_layout_set_font_description(label, font);
        Pos first = std::min(caret, anchor), last = std::max(caret, anchor);
        Pos current = caret_row();
        auto number = top_line;
        double left = text_left() - scroll_x();
        for (std::size_t i = 0; i < visible.size(); ++i) {
            auto& layout = visible[i];
            auto row = layout.row;
            int y = inset + static_cast<int>(i) * line_height;
            int baseline_top = y + (line_height - font_height) / 2;
            bool active = row.start == current;
            if (active) fill(cr, 0x1e242b, gutter + 1, y, width - gutter, line_height);
            bool continued = row.start && rows.bytes(row.start - 1, 1) != "\n";
            auto value = number ? (continued ? std::string("↪") : std::to_string(*number))
                                : hex_address(row.start);
            pango_layout_set_text(label, value.c_str(), -1);
            int label_width = 0;
            pango_layout_get_pixel_size(label, &label_width, nullptr);
            color(cr, active ? 0xa2b4ba : 0x62717d);
            cairo_move_to(cr, gutter - 11 - label_width, baseline_top);
            pango_cairo_show_layout(cr, label);
            cairo_save(cr);
            cairo_rectangle(cr, gutter + 1, y, width - gutter - 1, line_height);
            cairo_clip(cr);
            if (first < row.next && last > row.start) {
                int* ranges = nullptr;
                int count = 0;
                auto line = pango_layout_get_line_readonly(layout.pango.get(), 0);
                pango_layout_line_get_x_ranges(line, layout.display_index(first), layout.display_index(last),
                                               &ranges, &count);
                for (int j = 0; j < count; ++j) {
                    auto x1 = ranges[2 * j] / double(PANGO_SCALE);
                    auto x2 = ranges[2 * j + 1] / double(PANGO_SCALE);
                    fill(cr, 0x304a50, left + x1, y, x2 - x1, line_height);
                }
                g_free(ranges);
                if (row.newline && last > row.end)
                    fill(cr, 0x304a50, left + layout.x(row.end), y, cell, line_height);
            }
            color(cr, 0xd5dee4);
            cairo_move_to(cr, left, baseline_top);
            pango_cairo_show_layout(cr, layout.pango.get());
            if (active && gtk_widget_has_focus(area)) {
                int x = static_cast<int>(left) + layout.x(caret);
                fill(cr, 0x89ddc8, x, y + 3, 2, line_height - 6);
                GdkRectangle rect{x, y, cell, line_height};
                gtk_im_context_set_cursor_location(im, &rect);
                if (!preedit.empty()) {
                    pango_layout_set_text(label, preedit.c_str(), -1);
                    int w = 0;
                    pango_layout_get_pixel_size(label, &w, nullptr);
                    fill(cr, 0x28383d, x, y, w, line_height);
                    color(cr, 0xe3edea);
                    cairo_move_to(cr, x, baseline_top);
                    pango_cairo_show_layout(cr, label);
                    fill(cr, 0x89ddc8, x, y + line_height - 2, w, 1);
                }
            }
            cairo_restore(cr);
            if (number && row.newline) ++*number;
        }
        g_object_unref(label);
    }

    void modify(Pos pos, Pos count, std::string_view text) {
        doc.replace(pos, count, text);
        caret_display_row.reset();
        rows.invalidate();
        if (pos < top || (pos == top && top != 0)) {
            top = 0;
            top_line = 1;
        }
        needs_layout = needs_bottom = true;
        widest = 0;
    }
    void edit(Pos pos, Pos count, std::string_view text, bool typing = false) {
        if (!enabled() || (!count && text.empty())) return;
        bool keep = count <= history_limit && text.size() + sizeof(Edit) <= history_limit - count;
        Edit entry{pos, caret, anchor, {}, {}};
        if (keep) {
            entry.before = doc.read(pos, count);
            entry.after = text;
        }
        modify(pos, count, text);
        for (const auto& item : future) history_bytes -= item.cost();
        future.clear();
        if (keep) {
            if (typing && consecutive && !count && !past.empty() && past.back().before.empty()
                && pos == past.back().pos + past.back().after.size()
                && past.back().after.size() + text.size() < 4096) {
                past.back().after += text;
                history_bytes += text.size();
            } else {
                history_bytes += entry.cost();
                past.push_back(std::move(entry));
            }
            while (history_bytes > history_limit) {
                history_bytes -= past.front().cost();
                past.pop_front();
            }
        } else {
            past.clear();
            history_bytes = 0;
        }
        consecutive = typing && count == 0;
        caret = anchor = pos + text.size();
        preferred_x = -1;
        changed();
    }
    void insert(std::string_view text, bool typing = false) {
        auto [a, b] = std::minmax(caret, anchor);
        edit(a, b - a, text, typing);
    }
    void history(bool redo) {
        if (!enabled()) return;
        auto& source = redo ? future : past;
        auto& dest = redo ? past : future;
        if (source.empty()) return;
        const auto& e = source.back();
        if (redo) {
            modify(e.pos, e.before.size(), e.after);
            caret = anchor = e.pos + e.after.size();
        } else {
            modify(e.pos, e.after.size(), e.before);
            caret = e.cursor;
            anchor = e.anchor;
        }
        dest.push_back(std::move(source.back()));
        source.pop_back();
        preferred_x = -1;
        consecutive = false;
        changed();
    }
    bool copy() {
        if (caret == anchor) return false;
        auto [a, b] = std::minmax(caret, anchor);
        if (b - a > 16 * 1024 * 1024)
            throw std::runtime_error("Select up to 16 MiB at a time to copy to the clipboard.");
        auto text = doc.read(a, b - a);
        if (!g_utf8_validate(text.data(), text.size(), nullptr))
            throw std::runtime_error("The clipboard accepts UTF-8 text. Select text without binary bytes.");
        auto clipboard = gtk_widget_get_clipboard(area, GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(clipboard, text.data(), static_cast<int>(text.size()));
        return true;
    }
    void cut() { if (enabled() && copy()) insert(""); }
    struct PasteRequest { std::weak_ptr<Impl> view; Pos generation; };
    void paste() {
        if (!enabled()) return;
        auto cb = gtk_widget_get_clipboard(area, GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_request_text(cb, +[](GtkClipboard*, const gchar* text, gpointer data) {
            std::unique_ptr<PasteRequest> request(static_cast<PasteRequest*>(data));
            auto s = request->view.lock();
            if (!s || !text || !s->enabled() || s->generation != request->generation) return;
            s->guard([&] {
                std::string_view incoming(text);
                if (incoming.size() > 16 * 1024 * 1024)
                    throw std::runtime_error("Paste up to 16 MiB of clipboard text at a time.");
                if (!incoming.empty()) s->insert(incoming);
                return TRUE;
            });
        }, new PasteRequest{weak_from_this(), generation});
    }

    int char_class(Pos pos) {
        if (pos == doc.size()) return -1;
        auto s = rows.bytes(pos, 4);
        auto ch = g_utf8_get_char_validated(s.data(), s.size());
        if (ch == '_' || g_unichar_isalnum(ch)) return 1;
        if (g_unichar_isspace(ch)) return 0;
        return 2;
    }
    Pos word_edge(Pos pos, bool right, bool skip_space) {
        Pos start = rows.start(pos);
        if (right) {
            if (pos >= doc.size()) return pos;
            Pos limit = rows.at(start).next;
            int category = char_class(pos);
            do { pos = rows.right(pos); } while (pos < limit && char_class(pos) == category);
            if (skip_space) while (pos < limit && char_class(pos) == 0) pos = rows.right(pos);
        } else {
            if (pos == start && pos) start = rows.start(pos - 1);
            if (skip_space)
                while (pos > start && char_class(rows.left(pos)) == 0) pos = rows.left(pos);
            if (pos > start) {
                int category = char_class(rows.left(pos));
                do { pos = rows.left(pos); }
                while (pos > start && char_class(rows.left(pos)) == category);
            }
        }
        return pos;
    }
    void vertical_move(int count, bool extend) {
        auto origin = caret_row();
        auto target = step_rows(origin, count);
        if (preferred_x < 0) preferred_x = make_layout(rows.at(origin)).x(caret);
        auto row = rows.at(target);
        auto pos = make_layout(row).hit(preferred_x);
        if (origin == target && count < 0) pos = 0;
        if (origin == target && count > 0) pos = doc.size();
        move(pos, extend, true, target);
    }
    gboolean key(GdkEventKey* e) {
        if (!enabled()) return FALSE;
        auto primary = gtk_widget_get_modifier_mask(area, GDK_MODIFIER_INTENT_PRIMARY_ACCELERATOR);
        bool ctrl = (e->state & primary) == primary;
        bool shift = e->state & GDK_SHIFT_MASK;
        bool alt = e->state & ((GDK_MOD1_MASK | GDK_SUPER_MASK | GDK_META_MASK) & ~primary);
        if (alt) return FALSE;
        auto keyval = e->keyval;
        if (ctrl) {
            switch (gdk_keyval_to_lower(keyval)) {
                case GDK_KEY_a: anchor = 0; move(doc.size(), true); return TRUE;
                case GDK_KEY_z: history(shift); return TRUE;
                case GDK_KEY_y: history(true); return TRUE;
                case GDK_KEY_x: cut(); return TRUE;
                case GDK_KEY_c: copy(); return TRUE;
                case GDK_KEY_v: paste(); return TRUE;
                case GDK_KEY_Insert: copy(); return TRUE;
            }
        }
#ifdef __APPLE__
        if (ctrl && (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down)) {
            move(keyval == GDK_KEY_Up ? 0 : doc.size(), shift);
            return TRUE;
        }
#endif
        if (!ctrl && gtk_im_context_filter_keypress(im, e)) return TRUE;
        switch (keyval) {
            case GDK_KEY_Left: case GDK_KEY_KP_Left:
                move(!shift && anchor != caret ? std::min(caret, anchor)
                    : ctrl ? word_edge(caret, false, true) : rows.left(caret), shift);
                break;
            case GDK_KEY_Right: case GDK_KEY_KP_Right:
                move(!shift && anchor != caret ? std::max(caret, anchor)
                    : ctrl ? word_edge(caret, true, true) : rows.right(caret), shift);
                break;
            case GDK_KEY_Up: case GDK_KEY_KP_Up: vertical_move(-1, shift); break;
            case GDK_KEY_Down: case GDK_KEY_KP_Down: vertical_move(1, shift); break;
            case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: vertical_move(-page_rows(), shift); break;
            case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: vertical_move(page_rows(), shift); break;
            case GDK_KEY_Home: case GDK_KEY_KP_Home: move(ctrl ? 0 : caret_row(), shift); break;
            case GDK_KEY_End: case GDK_KEY_KP_End: {
                if (ctrl) move(doc.size(), shift);
                else {
                    auto row = rows.at(caret_row());
                    move(row.end, shift, false, row.start);
                }
                break;
            }
            case GDK_KEY_Delete: case GDK_KEY_KP_Delete:
                if (shift && !ctrl) cut();
                else if (anchor != caret) insert("");
                else edit(caret, (ctrl ? word_edge(caret, true, true) : rows.right(caret)) - caret, "");
                break;
            case GDK_KEY_BackSpace:
                if (anchor != caret) insert("");
                else {
                    auto prev = ctrl ? word_edge(caret, false, true) : rows.left(caret);
                    edit(prev, caret - prev, "");
                }
                break;
            case GDK_KEY_Return: case GDK_KEY_KP_Enter:
                if (ctrl) return FALSE;
                insert(crlf ? "\r\n" : "\n");
                break;
            case GDK_KEY_Tab: case GDK_KEY_KP_Tab:
                if (ctrl || shift) return FALSE;
                insert("\t");
                break;
            case GDK_KEY_Insert: if (shift) paste(); else return FALSE; break;
            default: {
                if (ctrl) return FALSE;
                auto ch = gdk_keyval_to_unicode(keyval);
                if (!ch || g_unichar_iscntrl(ch)) return FALSE;
                char buffer[6];
                auto length = g_unichar_to_utf8(ch, buffer);
                insert(std::string_view(buffer, length), true);
            }
        }
        return TRUE;
    }
    struct Hit { Pos pos, row; };
    Hit hit(double x, double y) {
        sync();
        auto index = static_cast<int>(std::floor((y - inset) / line_height));
        if (index >= static_cast<int>(visible.size())) return {doc.size(), visible.back().row.start};
        const auto& layout = visible[std::max(0, index)];
        auto pos = x < text_left() ? layout.row.start : layout.hit(x - text_left() + scroll_x());
        return {pos, layout.row.start};
    }
    void drag_to(double x, double y) {
        auto [pos, row] = hit(x, std::clamp(y, 0., double(height - 1)));
        move(pos, true, false, row);
    }
    using Action = void (EditorView::*)();
    struct MenuAction { std::weak_ptr<Impl> view; Action action; };
    void build_context_menu() {
        context_menu = gtk_menu_new();
        g_object_ref_sink(context_menu);
        gtk_menu_attach_to_widget(GTK_MENU(context_menu), area, nullptr);
        const std::array<std::pair<const char*, Action>, 6> actions{{
            {"Undo", &EditorView::undo}, {"Redo", &EditorView::redo},
            {"Cut", &EditorView::cut}, {"Copy", &EditorView::copy},
            {"Paste", &EditorView::paste}, {"Select All", &EditorView::select_all}
        }};
        for (std::size_t i = 0; i < actions.size(); ++i) {
            if (i == 2 || i == 5)
                gtk_menu_shell_append(GTK_MENU_SHELL(context_menu), gtk_separator_menu_item_new());
            auto [name, action] = actions[i];
            auto item = gtk_menu_item_new_with_label(name);
            g_signal_connect_data(item, "activate", G_CALLBACK(+[](GtkWidget*, gpointer data) {
                auto binding = static_cast<MenuAction*>(data);
                auto view = binding->view.lock();
                if (!view || !view->enabled()) return;
                view->guard([&] { (view->owner.*binding->action)(); return TRUE; });
            }), new MenuAction{weak_from_this(), action}, +[](gpointer data, GClosure*) {
                delete static_cast<MenuAction*>(data);
            }, GConnectFlags(0));
            gtk_menu_shell_append(GTK_MENU_SHELL(context_menu), item);
            menu_items[i] = item;
        }
        gtk_widget_show_all(context_menu);
    }
    void popup(GdkEventButton* event) {
        if (!context_menu) build_context_menu();
        auto clipboard = gtk_widget_get_clipboard(area, GDK_SELECTION_CLIPBOARD);
        std::array<bool, 6> available{!past.empty(), !future.empty(), caret != anchor,
                                      caret != anchor, bool(gtk_clipboard_wait_is_text_available(clipboard)),
                                      doc.size() != 0};
        for (std::size_t i = 0; i < available.size(); ++i)
            gtk_widget_set_sensitive(menu_items[i], available[i]);
        gtk_menu_popup_at_pointer(GTK_MENU(context_menu), reinterpret_cast<GdkEvent*>(event));
    }
    gboolean press(GdkEventButton* e) {
        bool context = gdk_event_triggers_context_menu(reinterpret_cast<GdkEvent*>(e));
        if ((!context && e->button != 1) || !enabled()) return FALSE;
        gtk_widget_grab_focus(area);
        gtk_im_context_reset(im);
        auto [pos, start] = hit(e->x, e->y);
        if (context) {
            stop_drag();
            if (caret == anchor || pos < std::min(caret, anchor) || pos > std::max(caret, anchor))
                move(pos, false, false, start);
            popup(e);
            return TRUE;
        }
        if (e->type == GDK_3BUTTON_PRESS || e->x < gutter) {
            auto row = rows.at(start);
            anchor = row.start;
            move(row.next, true);
        } else if (e->type == GDK_2BUTTON_PRESS) {
            if (pos < doc.size()) {
                int category = char_class(pos);
                Pos prev = rows.left(pos);
                anchor = pos && char_class(prev) == category ? word_edge(pos, false, false) : pos;
                move(word_edge(pos, true, false), true);
            }
        } else {
            move(pos, e->state & GDK_SHIFT_MASK, false, start);
        }
        dragging = true;
        return TRUE;
    }
    void stop_drag() {
        dragging = false;
        if (drag_timer) { g_source_remove(drag_timer); drag_timer = 0; }
    }
    gboolean motion(double x, double y) {
        if (!dragging) return FALSE;
        drag_x = x;
        drag_y = y;
        if ((y < 0 || y >= height) && !drag_timer) {
            drag_timer = g_timeout_add(40, +[](gpointer data) -> gboolean {
                auto s = static_cast<Self*>(data);
                if (!s->enabled() || !s->dragging || (s->drag_y >= 0 && s->drag_y < s->height)) {
                    s->drag_timer = 0;
                    return G_SOURCE_REMOVE;
                }
                return s->guard([&] {
                    s->scroll_rows(s->drag_y < 0 ? -1 : 1);
                    s->drag_to(s->drag_x, s->drag_y);
                    return G_SOURCE_CONTINUE;
                });
            }, this);
        }
        drag_to(x, y);
        return TRUE;
    }
    void scroll_rows(int count) {
        sync();
        set_top(std::min(bottom, step_rows(top, count)));
        gtk_widget_queue_draw(area);
    }
    gboolean scroll(GdkEventScroll* e) {
        double dx = 0, dy = 0;
        if (e->direction == GDK_SCROLL_SMOOTH) gdk_event_get_scroll_deltas(reinterpret_cast<GdkEvent*>(e), &dx, &dy);
        else if (e->direction == GDK_SCROLL_UP) dy = -1;
        else if (e->direction == GDK_SCROLL_DOWN) dy = 1;
        else if (e->direction == GDK_SCROLL_LEFT) dx = -1;
        else if (e->direction == GDK_SCROLL_RIGHT) dx = 1;
        if (e->state & GDK_SHIFT_MASK) { dx += dy; dy = 0; }
        if (dx) gtk_adjustment_set_value(horizontal, scroll_x() + dx * cell * 4);
        wheel += dy * 3;
        int steps = std::clamp(static_cast<int>(wheel), -page_rows() * 3, page_rows() * 3);
        if (steps) {
            wheel -= static_cast<int>(wheel);
            scroll_rows(steps);
        }
        return TRUE;
    }
};

EditorView::EditorView(Document& doc) : impl_(std::make_shared<Impl>(*this, doc)) {
    impl_->reset();
}
EditorView::~EditorView() = default;
GtkWidget* EditorView::widget() const { return impl_->root; }
void EditorView::focus() {
    gtk_widget_grab_focus(impl_->area);
    gtk_widget_queue_draw(impl_->area);
}
void EditorView::reset() { impl_->reset(); }
void EditorView::jump(Pos pos, std::size_t length) {
    gtk_im_context_reset(impl_->im);
    pos = std::min(pos, impl_->doc.size());
    impl_->anchor = pos;
    impl_->move(pos + std::min<Pos>(length, impl_->doc.size() - pos), true);
}
Pos EditorView::cursor() const { return impl_->caret; }
Pos EditorView::selection_size() const {
    return std::max(impl_->anchor, impl_->caret) - std::min(impl_->anchor, impl_->caret);
}
void EditorView::undo() { impl_->history(false); }
void EditorView::redo() { impl_->history(true); }
void EditorView::cut() { impl_->cut(); }
void EditorView::copy() { impl_->copy(); }
void EditorView::paste() { impl_->paste(); }
void EditorView::select_all() {
    impl_->anchor = 0;
    impl_->move(impl_->doc.size(), true);
}

} // namespace slate
