#include "markdown_preview.hpp"
#include "mermaid_renderer.hpp"
#include "theme.hpp"

#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <vector>

namespace {
namespace fs = std::filesystem;
using slate::Document;
using slate::MarkdownPreview;

void check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

GtkWidget* named(GtkWidget* widget, const char* name) {
    if (g_strcmp0(gtk_widget_get_name(widget), name) == 0) return widget;
    if (!GTK_IS_CONTAINER(widget)) return nullptr;
    auto children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget* result = nullptr;
    for (auto* it = children; it && !result; it = it->next) result = named(GTK_WIDGET(it->data), name);
    g_list_free(children);
    return result;
}

void wait(MarkdownPreview& preview) {
    auto deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (!g_object_get_data(G_OBJECT(preview.widget()), "markdown-ready")) {
        check(g_get_monotonic_time() < deadline, "preview timed out");
        g_main_context_iteration(nullptr, FALSE);
        g_usleep(1000);
    }
}

std::string rendered(GtkTextView* view) {
    auto* buffer = gtk_text_view_get_buffer(view);
    GtkTextIter first, last;
    gtk_text_buffer_get_bounds(buffer, &first, &last);
    auto* text = gtk_text_buffer_get_text(buffer, &first, &last, TRUE);
    std::string result(text);
    g_free(text);
    return result;
}

GtkTextIter find(GtkTextView* view, const char* text) {
    GtkTextIter start, found;
    gtk_text_buffer_get_start_iter(gtk_text_view_get_buffer(view), &start);
    check(gtk_text_iter_forward_search(&start, text, GTK_TEXT_SEARCH_TEXT_ONLY, &found, nullptr, nullptr),
          "text missing from preview");
    return found;
}

void has_tag(GtkTextView* view, const char* text, const char* tag) {
    auto iter = find(view, text);
    auto table = gtk_text_buffer_get_tag_table(gtk_text_view_get_buffer(view));
    check(gtk_text_iter_has_tag(&iter, gtk_text_tag_table_lookup(table, tag)), tag);
}

std::vector<GtkWidget*> diagram_children(GtkTextView* view) {
    std::vector<GtkWidget*> children;
    GtkTextIter iter;
    gtk_text_buffer_get_start_iter(gtk_text_view_get_buffer(view), &iter);
    do {
        if (auto* anchor = gtk_text_iter_get_child_anchor(&iter)) {
            auto* widgets = gtk_text_child_anchor_get_widgets(anchor);
            check(widgets && !widgets->next, "one widget per Mermaid anchor");
            auto* box = GTK_WIDGET(widgets->data);
            check(g_str_equal(gtk_widget_get_name(box), "markdown-mermaid"), "Mermaid block anchor");
            children.push_back(gtk_bin_get_child(GTK_BIN(box)));
            g_list_free(widgets);
        }
    } while (gtk_text_iter_forward_char(&iter));
    return children;
}

void mermaid_fences(MarkdownPreview& preview, GtkTextView* view) {
    Document doc;
    doc.replace(0, 0, "```mermaid\nflowchart LR\nSource --> Preview\n```\n\n"
                      "~~~mermaid\nunknown diagram syntax\n~~~\n\n"
                      "```text\nflowchart LR\nOrdinary --> Fence\n```\n");
    preview.show(doc.snapshot());
    wait(preview);
    has_tag(view, "Ordinary --> Fence", "md-code-block");
    const auto text = rendered(view);
    check(text.find("Source --> Preview") == text.npos && text.find("unknown diagram syntax") == text.npos,
          "Mermaid fences use widget anchors");

    // The page becomes selectable before the diagram worker finishes.
    const auto deadline = g_get_monotonic_time() + 12 * G_TIME_SPAN_SECOND;
    std::vector<GtkWidget*> children;
    for (;;) {
        children = diagram_children(view);
        if (children.size() == 2 && children[0] && !GTK_IS_LABEL(children[0]) &&
            children[1] && !GTK_IS_LABEL(children[1])) break;
        check(g_get_monotonic_time() < deadline, "Mermaid preview widgets timed out");
        g_main_context_iteration(nullptr, FALSE);
        g_usleep(1000);
    }
    const bool available = slate::mermaid_renderer_available();
    if (available) {
        check(GTK_IS_IMAGE(children[0]) && g_str_equal(gtk_widget_get_name(children[0]), "markdown-mermaid-image"),
              "valid Mermaid fence rendered a GTK image");
        auto* pixbuf = gtk_image_get_pixbuf(GTK_IMAGE(children[0]));
        check(pixbuf && gdk_pixbuf_get_width(pixbuf) > 100 && gdk_pixbuf_get_height(pixbuf) > 40,
              "rendered diagram pixbuf");
    } else {
        check(GTK_IS_EXPANDER(children[0]), "missing renderer offers diagram source");
    }
    check(GTK_IS_EXPANDER(children[1]), "invalid Mermaid fence offers its source");
    auto* expander = GTK_EXPANDER(children[1]);
    check(g_str_equal(gtk_expander_get_label(expander), available ? "Could not render Mermaid diagram"
                                                                  : "Mermaid renderer unavailable"),
          "diagram fallback explains its status");
    gtk_expander_set_expanded(expander, TRUE);
    auto* source = gtk_bin_get_child(GTK_BIN(expander));
    check(GTK_IS_LABEL(source) && gtk_label_get_selectable(GTK_LABEL(source)) &&
          g_str_equal(gtk_label_get_text(GTK_LABEL(source)), "unknown diagram syntax"),
          "fallback exposes original selectable Mermaid source");

    auto* buffer = gtk_text_view_get_buffer(view);
    auto first = find(view, "Ordinary"), last = first;
    gtk_text_iter_forward_word_end(&last);
    gtk_text_buffer_select_range(buffer, &first, &last);
    auto* original = available ? gtk_image_get_pixbuf(GTK_IMAGE(children[0])) : nullptr;
    if (original) g_object_ref(original);
    auto light = slate::default_palette(false);
    light.code_text = "#4931a0";
    preview.set_palette(light);
    check(gtk_text_view_get_buffer(view) == buffer, "changing appearance keeps the current preview buffer");
    auto* tags = gtk_text_buffer_get_tag_table(buffer);
    GdkRGBA *foreground = nullptr, expected{};
    g_object_get(gtk_text_tag_table_lookup(tags, "md-code-block"), "foreground-rgba", &foreground, nullptr);
    gdk_rgba_parse(&expected, light.code_text.c_str());
    check(foreground && gdk_rgba_equal(foreground, &expected), "custom code color applied to current document");
    gdk_rgba_free(foreground);
    if (original) {
        const auto timeout = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
        for (;;) {
            auto widgets = diagram_children(view);
            if (!widgets.empty() && GTK_IS_IMAGE(widgets[0])) {
                auto* current = gtk_image_get_pixbuf(GTK_IMAGE(widgets[0]));
                if (current && current != original && gdk_pixbuf_get_has_alpha(current)) break;
            }
            check(g_get_monotonic_time() < timeout, "theme switch did not refresh the diagram");
            g_main_context_iteration(nullptr, FALSE);
            g_usleep(1000);
        }
        g_object_unref(original);
    }
    check(gtk_text_view_get_buffer(view) == buffer && gtk_text_buffer_get_selection_bounds(buffer, &first, &last),
          "changing appearance preserves the rendered text and selection");
    auto* selected = gtk_text_buffer_get_text(buffer, &first, &last, FALSE);
    check(g_str_equal(selected, "Ordinary"), "selected text preserved through theme switch");
    g_free(selected);
}

GdkRectangle location(GtkTextView* view, const char* value, bool end = false) {
    auto iter = find(view, value);
    if (end) gtk_text_iter_forward_chars(&iter, g_utf8_strlen(value, -1));
    GdkRectangle rect{};
    gtk_text_view_get_iter_location(view, &iter, &rect);
    return rect;
}

void wait_for_layout(GtkTextView* view) {
    bool painted = false;
    auto handler = g_signal_connect_after(view, "draw", G_CALLBACK(+[](GtkWidget*, cairo_t*, bool* flag) -> gboolean {
        *flag = true;
        return FALSE;
    }), &painted);
    gtk_widget_queue_draw(GTK_WIDGET(view));
    const auto deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    while (!painted && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(nullptr, FALSE);
        g_usleep(1000);
    }
    g_signal_handler_disconnect(view, handler);
    check(painted, "table layout drawn");
}

void mermaid_resize(MarkdownPreview& preview, GtkTextView* view, GtkWindow* window) {
    if (!slate::mermaid_renderer_available()) return;
    Document doc;
    doc.replace(0, 0, "```mermaid\nflowchart LR\n A[Open file] --> B[Parse Markdown] --> C[Draw preview]\n```\n\n"
                      "```mermaid\nsequenceDiagram\n Editor->>Preview: Parse document\n Preview-->>Editor: Ready\n```\n\nAfter diagrams\n");
    preview.show(doc.snapshot());
    wait(preview);
    auto allocation = [&](int requested, int expected_width = 0) {
        gtk_window_resize(window, requested, 460);
        const auto deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
        for (;;) {
            wait_for_layout(view);
            auto widgets = diagram_children(view);
            const int width = gtk_widget_get_allocated_width(GTK_WIDGET(view));
            if (std::abs(width - requested) > 16 || widgets.size() != 2 ||
                !GTK_IS_IMAGE(widgets[0]) || !GTK_IS_IMAGE(widgets[1])) {
                check(g_get_monotonic_time() < deadline, "diagram viewport did not settle");
                continue;
            }
            GtkAllocation a{}, b{};
            gtk_widget_get_allocation(gtk_widget_get_parent(widgets[0]), &a);
            gtk_widget_get_allocation(gtk_widget_get_parent(widgets[1]), &b);
            const auto right = width - gtk_text_view_get_right_margin(view);
            if (a.x + a.width <= right && b.x + b.width <= right && a.y + a.height <= b.y &&
                a.width > 100 && b.height > 100 && (!expected_width || a.width == expected_width)) return a;
            check(g_get_monotonic_time() < deadline, "diagram clipped or overlapping its neighbor");
        }
    };
    const auto wide = allocation(760);
    const auto narrow = allocation(360);
    check(narrow.width < wide.width && std::abs(narrow.height * wide.width - wide.height * narrow.width) <= wide.width,
          "diagram did not scale proportionally");
    check(allocation(760, wide.width).height == wide.height, "diagram did not return to full size");
}

void follow(GtkTextView* view, const char* text) {
    auto iter = find(view, text);
    gtk_text_buffer_place_cursor(gtk_text_view_get_buffer(view), &iter);
    auto* event = gdk_event_new(GDK_KEY_PRESS);
    event->key.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(GTK_WIDGET(view))));
    event->key.keyval = GDK_KEY_Return;
    gboolean handled = FALSE;
    g_signal_emit_by_name(view, "key-press-event", &event->key, &handled);
    gdk_event_free(event);
    check(handled, "activate rendered link");
}

void exercise() {
    char tmp[] = "/tmp/slate-preview-XXXXXX";
    check(mkdtemp(tmp), "create test directory");
    const fs::path root(tmp);
    struct Clean { fs::path path; ~Clean() { std::error_code e; fs::remove_all(path, e); } } clean{root};
    auto* image = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 5, 6);
    gdk_pixbuf_fill(image, 0x83cbdfff);
    check(gdk_pixbuf_save(image, (root / "test.png").c_str(), "png", nullptr, nullptr), "write PNG");
    g_object_unref(image);
    std::ofstream(root / "example.md") <<
        "# Hello world\n\n[Go][ref] · [Open another](other%20one.md#anchor)\n\n"
        "> A **bold** and *slanted* quote with `inline code`.\n\n"
        "- [x] done\n  - child\n\n~~~cpp\nint answer = 42;\n~~~\n\n"
        "| Name | Quantity |\n| :--- | ---: |\n| Biscuit | 2 |\n\n"
        "## Destination\n\n![logo](test.png) ![missing image](absent.png)\n\n[ref]: #destination\n";
    Document doc;
    doc.open(root / "example.md");
    auto* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 760, 460);
    MarkdownPreview preview;
    gtk_container_add(GTK_CONTAINER(window), preview.widget());
    gtk_widget_show_all(window);
    auto* view = GTK_TEXT_VIEW(named(preview.widget(), "markdown-text"));
    check(view && !gtk_text_view_get_editable(view), "read-only native text view");
    preview.show(doc.snapshot());
    wait(preview);
    auto text = rendered(view);
    has_tag(view, "Hello world", "md-h1");
    has_tag(view, "slanted", "md-em");
    has_tag(view, "bold", "md-strong");
    has_tag(view, "bold", "md-quote");
    has_tag(view, "inline code", "md-code");
    has_tag(view, "int answer", "md-code-block");
    has_tag(view, "Go", "md-link");
    has_tag(view, "Quantity", "md-table-head");
    has_tag(view, "Biscuit", "md-cell");
    check(text.find("☑") != text.npos, "task rendering");
    check(text.find("**") == text.npos && text.find("[ref]") == text.npos, "Markdown syntax consumed");
    has_tag(view, "missing image", "md-alt");
    auto* buffer = gtk_text_view_get_buffer(view);
    GtkTextIter iter;
    gtk_text_buffer_get_start_iter(buffer, &iter);
    bool got_image = false;
    do { if (gtk_text_iter_get_pixbuf(&iter)) got_image = true; } while (gtk_text_iter_forward_char(&iter));
    check(got_image, "local PNG embedded");
    follow(view, "Go");
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, gtk_text_buffer_get_insert(buffer));
    auto heading = find(view, "Destination");
    check(gtk_text_iter_equal(&iter, &heading), "cross-document reference scrolls to heading");
    bool opened = false;
    preview.set_open_link([&](const fs::path& path, const std::string& fragment) {
        opened = path == root / "other one.md" && fragment == "anchor";
    });
    follow(view, "Open another");
    check(opened, "decoded relative Markdown link routed to editor");
    mermaid_fences(preview, view);
    mermaid_resize(preview, view, GTK_WINDOW(window));

    Document table_doc;
    table_doc.replace(0, 0,
        "| Right | Center | Details |\n| ---: | :---: | :--- |\n"
        "| 123456 | **Wide label** | Alpha line [link](#table-end) with a long description that "
        "continues across multiple visual lines inside this cell and ends with a final note. |\n"
        "| 27 | 界 | Short row |\n| 910 |\n"
        "| 83 | Picture | Before image ![a small image](" + (root / "test.png").string() +
        ") the text after it wraps within the column and ends with this caption. |\n\n## Table end\n");
    gtk_window_resize(GTK_WINDOW(window), 460, 400);
    const auto resize_deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    while (gtk_widget_get_allocated_width(GTK_WIDGET(view)) > 500) {
        check(g_get_monotonic_time() < resize_deadline, "narrow preview allocation");
        g_main_context_iteration(nullptr, FALSE);
        g_usleep(1000);
    }
    preview.show(table_doc.snapshot());
    wait(preview);
    wait_for_layout(view);
    // GtkTextBuffer is complete before the next GtkTextView layout/paint.
    bool painted = false;
    auto draw = g_signal_connect_after(view, "draw", G_CALLBACK(+[](GtkWidget*, cairo_t*, bool* seen) -> gboolean {
        *seen = true;
        return FALSE;
    }), &painted);
    gtk_widget_queue_draw(GTK_WIDGET(view));
    const auto paint_deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    while (!painted) {
        check(g_get_monotonic_time() < paint_deadline, "narrow preview paint");
        g_main_context_iteration(nullptr, FALSE);
        g_usleep(500);
    }
    g_signal_handler_disconnect(view, draw);
    const auto alpha = location(view, "Alpha line");
    const auto final = location(view, "note.");
    const auto next_row = location(view, "Short row");
    check(final.y > alpha.y && next_row.y > final.y, "wide cells wrap before the next row");
    check(alpha.x == next_row.x && final.x >= alpha.x, "wrapped cells keep their own column");
    check(location(view, "caption.").y > location(view, "Before image").y, "prose wraps around a table image");
    gtk_text_buffer_get_start_iter(gtk_text_view_get_buffer(view), &iter);
    int table_images = 0;
    do {
        if (gtk_text_iter_get_pixbuf(&iter)) ++table_images;
    } while (gtk_text_iter_forward_char(&iter));
    check(table_images == 1, "wrapped image cell embeds a single image");
    check(location(view, "Details").x == alpha.x, "heading and styled data share the column");
    check(location(view, "123456", true).x == location(view, "27", true).x,
          "right alignment is based on actual text width");
    const auto wide_center = location(view, "Wide label").x + location(view, "Wide label", true).x;
    const auto small_center = location(view, "界").x + location(view, "界", true).x;
    check(std::abs(wide_center - small_center) <= 2, "bold and wide Unicode text align to the same center");
    has_tag(view, "Wide label", "md-strong");
    follow(view, "link");
    buffer = gtk_text_view_get_buffer(view);
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, gtk_text_buffer_get_insert(buffer));
    const auto table_end = find(view, "Table end");
    check(gtk_text_iter_equal(&iter, &table_end), "links in wrapped table cells remain active");

    // A reference can be defined well beyond the size of a large-file page
    // when the document itself fits the ordinary preview budget.
    std::string reference = "[Start here][later]\n\n";
    for (int i = 0; i < 1000; ++i)
        reference += "A regular paragraph of text that leaves room for selecting formatted words.\n\n";
    reference += "## Far destination\n\n[later]: #far-destination\n";
    Document regular;
    regular.replace(0, 0, reference);
    preview.show(regular.snapshot());
    wait(preview);
    follow(view, "Start here");
    buffer = gtk_text_view_get_buffer(view);
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, gtk_text_buffer_get_insert(buffer));
    heading = find(view, "Far destination");
    check(gtk_text_iter_equal(&iter, &heading), "far reference resolved across source chunks");
    auto end = heading;
    gtk_text_iter_forward_chars(&end, 15);
    gtk_text_buffer_select_range(buffer, &heading, &end);
    auto* clipboard = gtk_widget_get_clipboard(GTK_WIDGET(view), GDK_SELECTION_CLIPBOARD);
    gtk_text_buffer_copy_clipboard(buffer, clipboard);
    auto* copied = gtk_clipboard_wait_for_text(clipboard);
    check(g_strcmp0(copied, "Far destination") == 0, "copy selected formatted text");
    g_free(copied);

    const auto huge = root / "large.md";
    std::uint64_t length = (std::uint64_t{5} << 30) + 1000;
    int fd = open(huge.c_str(), O_CREAT | O_RDWR, 0600);
    check(fd >= 0 && ftruncate(fd, length) == 0, "create large sparse Markdown");
    std::string head = "# First marker\n\n";
    std::string middle = "\n\n# Next marker\n\n";
    std::string tail = "\n\n# Last marker\n";
    check(pwrite(fd, head.data(), head.size(), 0) == (ssize_t)head.size(), "write first page");
    check(pwrite(fd, middle.data(), middle.size(), 8192 + 30) == (ssize_t)middle.size(), "write second page");
    check(pwrite(fd, tail.data(), tail.size(), length - tail.size()) == (ssize_t)tail.size(), "write last page");
    close(fd);
    Document large;
    large.open(huge);
    preview.show(large.snapshot());
    wait(preview);
    check(rendered(view).find("First marker") != std::string::npos, "first page displayed");
    auto* next = named(preview.widget(), "markdown-next");
    auto* prev = named(preview.widget(), "markdown-previous");
    check(gtk_widget_get_visible(next) && !gtk_widget_get_sensitive(prev), "first page controls");
    gtk_button_clicked(GTK_BUTTON(next));
    wait(preview);
    check(rendered(view).find("Next marker") != std::string::npos, "second page displayed");
    gtk_button_clicked(GTK_BUTTON(prev));
    wait(preview);
    has_tag(view, "First marker", "md-h1");
    preview.show(large.snapshot(), length);
    wait(preview);
    has_tag(view, "Last marker", "md-h1");
    check(!gtk_widget_get_sensitive(next) && gtk_widget_get_sensitive(prev), "last page controls");

    Document many;
    std::string lines;
    for (int i = 0; i < 20000; ++i) lines += "- A **bold** item\n";
    many.replace(0, 0, lines);
    preview.show(many.snapshot());
    wait(preview);
    check(gtk_widget_get_visible(next), "dense small file uses bounded pages");
    // A worker may see a shorter backing file than the open snapshot.
    const auto changed = root / "changed.md";
    std::ofstream(changed) << "# original";
    Document stale;
    stale.open(changed);
    fs::resize_file(changed, 0);
    preview.show(stale.snapshot());
    wait(preview);
    auto* status = named(preview.widget(), "markdown-status");
    check(std::string(gtk_label_get_text(GTK_LABEL(status))).find("shortened") != std::string::npos,
          "read failure shown without losing the preview widget");

    doc.replace(0, doc.size(), "# Updated\n");
    preview.show(many.snapshot());
    preview.show(large.snapshot(), length);
    preview.clear();
    preview.show(doc.snapshot());
    wait(preview);
    check(rendered(view) == "Updated", "superseded snapshots cannot replace latest text");
    preview.show(large.snapshot());
    gtk_widget_destroy(window);
    for (int i = 0; i < 40; ++i) { g_main_context_iteration(nullptr, FALSE); g_usleep(1000); }
}
} // namespace

int main(int argc, char** argv) {
    if (!gtk_init_check(&argc, &argv)) return 77;
    try { exercise(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
