#include "theme.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

GtkWidget* control(GtkWidget* widget, const char* name) {
    if (g_strcmp0(atk_object_get_name(gtk_widget_get_accessible(widget)), name) == 0 &&
        (GTK_IS_COMBO_BOX(widget) || GTK_IS_COLOR_BUTTON(widget))) return widget;
    if (!GTK_IS_CONTAINER(widget)) return nullptr;
    auto* children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget* found = nullptr;
    for (auto* it = children; it && !found; it = it->next) found = control(GTK_WIDGET(it->data), name);
    g_list_free(children);
    return found;
}

void edit(slate::Appearance& appearance, const char* mode, const char* background, bool save) {
    struct Action {
        const char* mode;
        const char* background;
        bool save;
        const char* error = nullptr;
    } action{mode, background, save};
    g_idle_add(+[](gpointer data) -> gboolean {
        auto& action = *static_cast<Action*>(data);
        auto* windows = gtk_window_list_toplevels();
        GtkWidget* dialog = nullptr;
        for (auto* it = windows; it; it = it->next)
            if (GTK_IS_DIALOG(it->data) && g_strcmp0(gtk_window_get_title(GTK_WINDOW(it->data)), "Appearance") == 0)
                dialog = GTK_WIDGET(it->data);
        g_list_free(windows);
        if (!dialog) return G_SOURCE_CONTINUE;
        auto* theme = control(dialog, "Theme");
        auto* swatch = control(dialog, "Document background");
        if (!theme || !swatch) {
            action.error = "Appearance dialog is missing its theme or color control";
        } else {
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(theme), action.mode);
            if (action.background) {
                GdkRGBA color{};
                gdk_rgba_parse(&color, action.background);
                gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(swatch), &color);
                g_signal_emit_by_name(swatch, "color-set");
            }
        }
        gtk_dialog_response(GTK_DIALOG(dialog), action.save ? GTK_RESPONSE_ACCEPT : GTK_RESPONSE_CANCEL);
        return G_SOURCE_REMOVE;
    }, &action);
    slate::Palette last = appearance.palette();
    appearance.edit(nullptr, [&](const slate::Palette& palette) { last = palette; });
    if (action.error) throw std::runtime_error(action.error);
    check(last == appearance.palette(), "Live appearance differs from saved or restored palette");
}

void exercise() {
    {
        slate::Appearance appearance;
        check(appearance.palette() == slate::default_palette(true), "First launch should use dark mode");
        edit(appearance, "light", "#fff5e2", true);
        check(!appearance.palette().dark && appearance.palette().background == "#fff5e2", "Save light colors");
        edit(appearance, "dark", "#231325", false);
        check(!appearance.palette().dark && appearance.palette().background == "#fff5e2", "Cancel restores light colors");
    }
    slate::Appearance reopened;
    check(!reopened.palette().dark && reopened.palette().background == "#fff5e2", "Preferences survive restart");
    edit(reopened, "dark", nullptr, true);
    check(reopened.palette() == slate::default_palette(true), "Cancelled dark colors were discarded");
    edit(reopened, "light", nullptr, true);
    check(reopened.palette().background == "#fff5e2", "Light retains its own custom colors");
}
} // namespace

int main(int argc, char** argv) {
    char directory[] = "/tmp/slate-appearance-XXXXXX";
    if (!mkdtemp(directory)) return 1;
    struct Cleanup {
        const char* path;
        ~Cleanup() { std::error_code e; std::filesystem::remove_all(path, e); }
    } cleanup{directory};
    g_setenv("XDG_CONFIG_HOME", directory, TRUE);
    if (!gtk_init_check(&argc, &argv)) return 77;
    try { exercise(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
