#include "document.hpp"
#include "editor_view.hpp"

#include <gtk/gtk.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t log_offset = 5ULL << 30;

void measure(std::size_t row_bytes) {
    // Put distinct log lines after a sparse prefix so byte offsets exceed 32 bits.
    char path[] = "/tmp/slate-viewport-XXXXXX";
    auto fd = mkstemp(path);
    if (fd < 0) throw std::runtime_error("Could not create benchmark file");
    std::string contents;
    for (unsigned row = 0; row < 1500; ++row) {
        std::string line = "INFO service=storage record=" + std::to_string(row) + " value=";
        line.resize(row_bytes - 1, static_cast<char>('a' + row % 26));
        contents += line + '\n';
    }
    auto written = pwrite(fd, contents.data(), contents.size(), static_cast<off_t>(log_offset));
    close(fd);
    if (written != static_cast<ssize_t>(contents.size())) {
        unlink(path);
        throw std::runtime_error("Could not write benchmark data");
    }
    slate::Document document;
    document.open(path);
    unlink(path);

    slate::EditorView view(document);
    auto* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 720);
    gtk_container_add(GTK_CONTAINER(window), view.widget());
    gtk_widget_show_all(window);
    view.focus();
    while (gtk_events_pending()) gtk_main_iteration();
    auto* canvas = gtk_grid_get_child_at(GTK_GRID(view.widget()), 0, 0);
    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
        gtk_widget_get_allocated_width(canvas), gtk_widget_get_allocated_height(canvas));
    auto* cr = cairo_create(surface);
    for (bool up : {false, true}) {
        std::vector<double> layout, frame;
        for (unsigned step = 0; step < 600; ++step) {
            auto row = up ? 1200 - step : 100 + step;
            auto start = Clock::now();
            view.jump(log_offset + row * row_bytes);
            auto shaped = Clock::now();
            gboolean handled = FALSE;
            g_signal_emit_by_name(canvas, "draw", cr, &handled);
            auto painted = Clock::now();
            if (step >= 60) {
                layout.push_back(std::chrono::duration<double, std::micro>(shaped - start).count());
                frame.push_back(std::chrono::duration<double, std::micro>(painted - start).count());
            }
        }
        std::sort(layout.begin(), layout.end());
        std::sort(frame.begin(), frame.end());
        std::printf("%3zu B rows, %-4s  layout %.0f/%.0f us  layout + Cairo paint %.0f/%.0f us\n",
            row_bytes, up ? "up" : "down", layout[layout.size() / 2], layout[layout.size() * 95 / 100],
            frame[frame.size() / 2], frame[frame.size() * 95 / 100]);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    gtk_widget_destroy(window);
}
} // namespace

int main(int argc, char** argv) {
#ifdef __linux__
    g_setenv("GDK_GL", "disable", FALSE);
#endif
    if (!gtk_init_check(&argc, &argv)) {
        std::fputs("This benchmark requires a graphical session or xvfb-run.\n", stderr);
        return 77;
    }
    try {
        std::puts("Median/p95 of 540 one-row scrolls beyond 5 GiB, 900 × 720 window");
        measure(128);
        measure(768);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
