#include "markdown_preview.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

GtkWidget* text_view(GtkWidget* root) {
    if (GTK_IS_TEXT_VIEW(root)) return root;
    if (!GTK_IS_CONTAINER(root)) return nullptr;
    auto children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget* found = nullptr;
    for (auto* it = children; it && !found; it = it->next)
        found = text_view(GTK_WIDGET(it->data));
    g_list_free(children);
    return found;
}

template<class Predicate>
void wait_until(Predicate predicate, const char* message) {
    const auto deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
    do {
        g_main_context_iteration(nullptr, FALSE);
        if (predicate()) return;
        g_usleep(500);
    } while (g_get_monotonic_time() < deadline);
    throw std::runtime_error(message);
}

struct Geometry {
    int x, y, right;
    int center() const { return (x + right) / 2; }
};

bool locate(GtkTextView* view, const char* word, GtkTextIter& first, GtkTextIter& end) {
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(gtk_text_view_get_buffer(view), &start);
    return gtk_text_iter_forward_search(&start, word, GTK_TEXT_SEARCH_TEXT_ONLY, &first, &end, nullptr);
}

Geometry geometry(GtkTextView* view, const char* word) {
    GtkTextIter first, end;
    if (!locate(view, word, first, end)) throw std::runtime_error(std::string("Missing table text: ") + word);
    gtk_text_iter_backward_char(&end);
    GdkRectangle a{}, b{};
    gtk_text_view_get_iter_location(view, &first, &a);
    gtk_text_view_get_iter_location(view, &end, &b);
    return {a.x, a.y, b.x + b.width};
}

void tagged(GtkTextView* view, const char* word, const char* style) {
    GtkTextIter first, end;
    check(locate(view, word, first, end), "styled cell text missing");
    auto tags = gtk_text_buffer_get_tag_table(gtk_text_view_get_buffer(view));
    check(gtk_text_iter_has_tag(&first, gtk_text_tag_table_lookup(tags, style)), style);
}

bool ready(const slate::MarkdownPreview& preview) {
    return g_object_get_data(G_OBJECT(preview.widget()), "markdown-ready");
}

void run() {
    slate::MarkdownPreview preview;
    auto* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 460, 480);
    gtk_container_add(GTK_CONTAINER(window), preview.widget());
    gtk_widget_show_all(window);
    auto* view = GTK_TEXT_VIEW(text_view(preview.widget()));
    check(view != nullptr, "missing preview text view");
    wait_until([&] { return gtk_widget_get_allocated_width(GTK_WIDGET(view)) >= 450; }, "initial width");
    unsigned paints = 0;
    g_signal_connect_after(view, "draw", G_CALLBACK(+[](GtkWidget*, cairo_t*, unsigned* paints) -> gboolean {
        ++*paints;
        return FALSE;
    }), &paints);
    auto show = [&](const std::string& text) {
        slate::Document document;
        document.replace(0, 0, text);
        preview.show(document.snapshot());
        wait_until([&] { return ready(preview); }, "preview did not finish");
        const auto last_paint = paints;
        gtk_widget_queue_draw(GTK_WIDGET(view));
        wait_until([&] { return paints > last_paint; }, "table was not painted");
    };
    std::string table = "| Team | State | Value |\n| :--- | :---: | ---: |\n"
        "| Alpha | I | 7 |\n| Québec | Steady | 12,345 |\n"
        "| First ";
    for (int i = 0; i < 8; ++i) table += "one two three four five six ";
    table += "final **bold** [read](other.md) `code` endword | done | 99 |\n"
             "\nAfter table\n";

    show(table);
    auto alpha = geometry(view, "Alpha"), quebec = geometry(view, "Québec");
    auto center1 = geometry(view, "I"), center2 = geometry(view, "Steady");
    auto right1 = geometry(view, "7"), right2 = geometry(view, "12,345");
    check(std::abs(alpha.x - quebec.x) <= 2, "left alignment differs across table rows");
    check(std::abs(center1.center() - center2.center()) <= 3, "center-aligned cells have different centers");
    check(std::abs(right1.right - right2.right) <= 3, "right-aligned cells have different right edges");
    check(alpha.y == center1.y && alpha.y == right1.y, "ordinary row wrapped between columns");

    const auto first = geometry(view, "First");
    const auto narrow_height = geometry(view, "endword").y - first.y;
    check(narrow_height > 0 && geometry(view, "done").y == first.y, "long cell text must wrap inside its column");
    check(geometry(view, "After table").y > first.y + narrow_height, "next paragraph overlaps table");
    tagged(view, "State", "md-table-head");
    tagged(view, "bold", "md-strong");
    tagged(view, "read", "md-link");
    tagged(view, "code", "md-code");

    gtk_window_resize(GTK_WINDOW(window), 920, 480);
    wait_until([&] {
        if (gtk_widget_get_allocated_width(GTK_WIDGET(view)) < 800 || !ready(preview)) return false;
        auto height = geometry(view, "endword").y - geometry(view, "First").y;
        return height > 0 && height < narrow_height;
    }, "widening the preview did not reflow the table");
    const auto wide_height = geometry(view, "endword").y - geometry(view, "First").y;
    gtk_window_resize(GTK_WINDOW(window), 460, 480);
    wait_until([&] {
        if (gtk_widget_get_allocated_width(GTK_WIDGET(view)) > 500 || !ready(preview)) return false;
        return geometry(view, "endword").y - geometry(view, "First").y > wide_height;
    }, "narrowing the preview did not reflow the table");

    std::string header = "|", rule = "|", row = "|";
    for (int i = 0; i < 16; ++i) {
        header += " Column" + std::to_string(i) + " |";
        rule += " :--- |";
        row += i == 0 ? " early |" : i == 15 ? " late |" : " x |";
    }
    show(header + "\n" + rule + "\n" + row + "\n");
    auto early = geometry(view, "early"), late = geometry(view, "late");
    check(early.y == late.y && late.x > gtk_widget_get_allocated_width(GTK_WIDGET(view)),
          "wide table columns should share a row and extend past the viewport");
    auto* horizontal = gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(view));
    wait_until([&] { return gtk_adjustment_get_upper(horizontal) > gtk_adjustment_get_page_size(horizontal) + 100; },
               "wide table should scroll horizontally");

    gtk_widget_destroy(window);
}
} // namespace

int main(int argc, char** argv) {
    if (!gtk_init_check(&argc, &argv)) return 77;
    try { run(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
