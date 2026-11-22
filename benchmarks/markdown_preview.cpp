#include "document.hpp"
#include "markdown_preview.hpp"

#include <gtk/gtk.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double peak_mib() {
#ifdef __linux__
    std::ifstream status("/proc/self/status");
    for (std::string line; std::getline(status, line);) {
        long kib;
        if (std::sscanf(line.c_str(), "VmHWM: %ld kB", &kib) == 1) return kib / 1024.;
    }
#endif
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
#ifdef __APPLE__
    return r.ru_maxrss / (1024. * 1024.);
#else
    return r.ru_maxrss / 1024.;
#endif
}

struct Example {
    fs::path path;
    explicit Example(std::string_view kind) {
        auto pattern = (fs::temp_directory_path() / "slate-preview-XXXXXX").string();
        int fd = ::mkstemp(pattern.data());
        if (fd < 0) throw std::runtime_error("Cannot create benchmark fixture");
        ::close(fd);
        path = pattern;
        std::ofstream out(path, std::ios::binary);
        out.exceptions(std::ios::failbit | std::ios::badbit);
        const std::string section =
            "## Read and edit a file\n\n"
            "Open a **directory** and select a [document](notes.md) from the list. "
            "Choose *Preview* to read the formatted text.\n\n"
            "> Read the input in bounded windows.\n\n"
            "- [x] Open the file\n- [ ] Save the edits\n\n"
            "```cpp\nauto text = file.read(offset, count);\n```\n\n"
            "| Operation | Unit |\n| :--- | ---: |\n| Read | bytes |\n\n";
        if (kind == "sparse") {
            out << "# Start\n\n" << section;
            out.seekp(std::streamoff{5} << 30);
            out << "\n\n# Last page\n\nTail text\n";
        } else {
            size_t bytes = 0;
            std::string part;
            if (kind == "guide") { bytes = 8192; part = section; }
            else if (kind == "manual") { bytes = 512 * 1024; part = section; }
            else if (kind == "dense") { bytes = 1024 * 1024; part = "- *x*\n"; }
            else if (kind == "line") { bytes = 1000 * 1024; part.assign(1024, 'x'); }
            else throw std::invalid_argument("Choose guide, manual, dense, line or sparse");
            std::string text;
            text.reserve(bytes);
            while (text.size() + part.size() <= bytes) text += part;
            out.write(text.data(), text.size());
        }
        out.close();
    }
    ~Example() { std::error_code ec; fs::remove(path, ec); }
};

GtkWidget* named(GtkWidget* root, std::string_view name) {
    if (name == gtk_widget_get_name(root)) return root;
    if (!GTK_IS_CONTAINER(root)) return nullptr;
    auto children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget* found = nullptr;
    for (auto* it = children; it && !found; it = it->next) found = named(GTK_WIDGET(it->data), name);
    g_list_free(children);
    return found;
}

struct Sample { double content_ms, paint_ms, ready_ms, seek_ms, clear_ms, max_dispatch_ms; };

Sample measure(slate::MarkdownPreview& preview, const slate::Document& doc, uint64_t offset) {
    double first = -1, longest = 0;
    auto start = Clock::now();
    preview.show(doc.snapshot(), offset);
    auto* view = GTK_TEXT_VIEW(named(preview.widget(), "markdown-text"));
    if (!view) throw std::runtime_error("Preview text surface missing");
    struct Paint { Clock::time_point start; double ms; } paint{start, -1};
    auto signal = g_signal_connect_after(view, "draw", G_CALLBACK(+[](GtkTextView* text, cairo_t*, Paint* p) -> gboolean {
        if (p->ms < 0 && gtk_text_buffer_get_char_count(gtk_text_view_get_buffer(text)) > 0)
            p->ms = elapsed(p->start);
        return FALSE;
    }), &paint);
    auto pump = [&] {
        auto before = Clock::now();
        if (!g_main_context_iteration(nullptr, FALSE)) g_usleep(500);
        longest = std::max(longest, elapsed(before));
    };
    while (!g_object_get_data(G_OBJECT(preview.widget()), "markdown-ready")) {
        pump();
        if (first < 0 && gtk_text_buffer_get_char_count(gtk_text_view_get_buffer(view)) > 0)
            first = elapsed(start);
        if (elapsed(start) > 30000) throw std::runtime_error("Preview missed 30 s deadline");
    }
    auto ready = elapsed(start);
    if (first < 0) first = ready;
    gtk_widget_queue_draw(GTK_WIDGET(view));
    while (paint.ms < 0 && elapsed(start) < 30000) pump();
    g_signal_handler_disconnect(view, signal);
    if (paint.ms < 0) throw std::runtime_error("No rendered text frame appeared");
    // Ask GTK for the visible layout at the bottom and top of the rendered page.
    auto seek_start = Clock::now();
    auto* buffer = gtk_text_view_get_buffer(view);
    for (bool bottom : {true, false}) {
        GtkTextIter iter;
        if (bottom) gtk_text_buffer_get_end_iter(buffer, &iter);
        else gtk_text_buffer_get_start_iter(buffer, &iter);
        gtk_text_view_scroll_to_iter(view, &iter, 0, TRUE, 0, bottom ? 1 : 0);
        GdkRectangle rect{};
        gtk_text_view_get_iter_location(view, &iter, &rect);
    }
    double seek = elapsed(seek_start);
    auto leaving = Clock::now();
    preview.clear();
    return {first, paint.ms, ready, seek, elapsed(leaving), longest};
}

int main_impl(int argc, char** argv) {
    if (argc < 2 || argc > 3) throw std::invalid_argument("Usage: slate_preview_bench WORKLOAD [REPEATS]");
    int repeats = argc == 3 ? std::stoi(argv[2]) : 5;
    if (repeats < 1 || repeats > 100) throw std::invalid_argument("Choose 1 to 100 repeats");
    Example fixture(argv[1]);
    slate::Document doc;
    doc.open(fixture.path);
#ifdef __linux__
    g_setenv("GDK_GL", "disable", FALSE);
#endif
    if (!gtk_init_check(nullptr, nullptr)) throw std::runtime_error("GTK needs a display");
    // Measure immediate document seeks independently of theme scroll animations.
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, nullptr);
    auto* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 650);
    slate::MarkdownPreview preview;
    gtk_container_add(GTK_CONTAINER(window), preview.widget());
    gtk_widget_show_all(window);
    while (gtk_events_pending()) gtk_main_iteration();
    auto base = peak_mib();
    std::vector<Sample> samples;
    for (int i = 0; i < repeats; ++i) {
        samples.push_back(measure(preview, doc, std::string_view(argv[1]) == "sparse" && i % 2 ? doc.size()-5 : 0));
    }
    auto median = [&samples](double Sample::*member) {
        std::vector<double> values;
        for (auto sample : samples) values.push_back(sample.*member);
        std::sort(values.begin(), values.end());
        return values[values.size()/2];
    };
    double worst = 0;
    for (auto sample : samples) worst = std::max(worst, sample.max_dispatch_ms);
    std::printf("%s  bytes=%llu runs=%d  first-text=%.1f ms  first-paint=%.1f ms  ready=%.1f ms  seek=%.1f ms  clear=%.1f ms  "
                "max-dispatch=%.1f ms  peak=%.1f MiB (startup %.1f MiB)\n",
        argv[1], static_cast<unsigned long long>(doc.size()), repeats, median(&Sample::content_ms),
        median(&Sample::paint_ms), median(&Sample::ready_ms), median(&Sample::seek_ms), median(&Sample::clear_ms), worst, peak_mib(), base);
    preview.clear();
    gtk_widget_destroy(window);
    return 0;
}
}
int main(int argc, char** argv) {
    try { return main_impl(argc, argv); }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
