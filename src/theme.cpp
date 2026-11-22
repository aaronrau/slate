#include "theme.hpp"

#include <glib/gstdio.h>

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace slate {
namespace {

struct Colors {
    std::string background, surface, text, muted, accent, code;
};

Colors defaults(bool dark) {
    if (dark) return {"#101010", "#191919", "#e7e7e7", "#a6a6a6", "#c8c8c8", "#d2d2d2"};
    return {"#fbfbfb", "#efefef", "#252525", "#626262", "#484848", "#555555"};
}

struct ColorField {
    const char* key;
    const char* label;
    std::string Colors::* value;
};
constexpr std::array fields{
    ColorField{"Background", "Document background", &Colors::background},
    ColorField{"Surface", "Sidebar and panels", &Colors::surface},
    ColorField{"Text", "Text", &Colors::text},
    ColorField{"Muted", "Secondary text", &Colors::muted},
    ColorField{"Accent", "Links and accents", &Colors::accent},
    ColorField{"Code", "Code text", &Colors::code}
};

struct Settings {
    bool dark = true;
    Colors night = defaults(true);
    Colors day = defaults(false);
    Colors& colors() { return dark ? night : day; }
};

GdkRGBA rgba(const std::string& value) {
    GdkRGBA result{};
    gdk_rgba_parse(&result, value.c_str());
    return result;
}

std::string hex(const GdkRGBA& color) {
    char value[8];
    std::snprintf(value, sizeof(value), "#%02x%02x%02x",
        static_cast<unsigned>(std::lround(color.red * 255)),
        static_cast<unsigned>(std::lround(color.green * 255)),
        static_cast<unsigned>(std::lround(color.blue * 255)));
    return value;
}

std::string mix(const std::string& a, const std::string& b, double amount) {
    const auto from = rgba(a), to = rgba(b);
    return hex({std::lerp(from.red, to.red, amount), std::lerp(from.green, to.green, amount),
                std::lerp(from.blue, to.blue, amount), 1});
}

Palette palette_for(bool dark, const Colors& c) {
    return {dark, c.background, c.surface, mix(c.surface, c.text, dark ? .10 : .055), c.text, c.muted,
        mix(c.surface, c.text, .22), c.accent, mix(c.background, c.accent, dark ? .32 : .25),
        c.text, c.code, mix(c.background, c.text, .05)};
}

using KeyFile = std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)>;

std::filesystem::path settings_path() {
    return std::filesystem::path(g_get_user_config_dir()) / "slate" / "appearance.ini";
}

Settings load() {
    Settings settings;
    KeyFile file(g_key_file_new(), g_key_file_unref);
    if (!g_key_file_load_from_file(file.get(), settings_path().c_str(), G_KEY_FILE_NONE, nullptr))
        return settings;
    gchar* mode = g_key_file_get_string(file.get(), "Appearance", "Mode", nullptr);
    settings.dark = g_strcmp0(mode, "light") != 0;
    g_free(mode);
    for (bool dark : {true, false}) {
        auto& colors = dark ? settings.night : settings.day;
        for (const auto& field : fields) {
            gchar* value = g_key_file_get_string(file.get(), dark ? "Dark" : "Light", field.key, nullptr);
            GdkRGBA color{};
            if (value && gdk_rgba_parse(&color, value) && color.alpha == 1)
                colors.*(field.value) = hex(color);
            g_free(value);
        }
    }
    return settings;
}

void save(const Settings& settings) {
    const auto path = settings_path();
    if (g_mkdir_with_parents(path.parent_path().c_str(), 0700) != 0)
        throw std::runtime_error("Could not create Slate's settings directory: " +
                                 std::string(g_strerror(errno)));
    KeyFile file(g_key_file_new(), g_key_file_unref);
    g_key_file_set_string(file.get(), "Appearance", "Mode", settings.dark ? "dark" : "light");
    for (bool dark : {true, false}) {
        const auto& colors = dark ? settings.night : settings.day;
        for (const auto& field : fields)
            g_key_file_set_string(file.get(), dark ? "Dark" : "Light", field.key,
                                  (colors.*(field.value)).c_str());
    }
    GError* error = nullptr;
    if (!g_key_file_save_to_file(file.get(), path.c_str(), &error)) {
        std::string message = error->message;
        g_error_free(error);
        throw std::runtime_error(message);
    }
}

constexpr char css[] = R"CSS(
@define-color theme_bg_color @slate_surface;
@define-color theme_base_color @slate_bg;
@define-color theme_fg_color @slate_text;
@define-color theme_text_color @slate_text;
@define-color theme_selected_bg_color @slate_selection;
@define-color theme_selected_fg_color @slate_text;
@define-color borders @slate_border;
* { outline-color: @slate_accent; }
window.background, .background { background-color: @slate_surface; color: @slate_text; }
dialog .dialog-action-area { background-color: @slate_surface; }
headerbar, .titlebar { background: @slate_surface; color: @slate_text; border-color: @slate_border; }
menu, popover.background { background: @slate_raised; color: @slate_text; border-color: @slate_border; }
menuitem { color: @slate_text; }
menuitem:hover, menuitem:active { background: @slate_selection; color: @slate_text; }
menuitem:disabled, button:disabled, label:disabled { color: @slate_muted; }
menu separator { background: @slate_border; }
button, entry, spinbutton, combobox button { background: @slate_raised; color: @slate_text;
  border-color: @slate_border; text-shadow: none; box-shadow: none; }
button:hover, combobox button:hover { background: @slate_line; }
button:checked, button:active { background: @slate_selection; color: @slate_text; }
entry, spinbutton { background: @slate_bg; caret-color: @slate_accent; }
entry:focus, button:focus { border-color: @slate_accent; }
entry selection, label selection, textview text selection { background: @slate_selection; color: @slate_text; }
entry image { color: @slate_muted; }
button.link, link { color: @slate_accent; }
treeview.view, list, .view { background-color: @slate_bg; color: @slate_text; }
treeview.view:selected, row:selected { background-color: @slate_selection; color: @slate_text; }
scrollbar, scrollbar trough { background: @slate_surface; border-color: @slate_border; }
scrollbar slider { background: @slate_muted; border-color: transparent; }
scrollbar slider:hover { background: @slate_text; }
tooltip.background { background: @slate_raised; border: 1px solid @slate_border; }
tooltip label { color: @slate_text; }
#slate { background: @slate_bg; color: @slate_text; }
#main-menu { background: @slate_surface; border-bottom: 1px solid @slate_border; padding: 0 3px; }
#main-menu > menuitem { padding: 3px 9px; font-size: 13px; }
#main-menu > menuitem:hover { background: @slate_raised; box-shadow: inset 0 -2px @slate_accent; }
#slate paned > separator { background: @slate_border; min-width: 1px; }
#tab-strip { background: @slate_surface; border-bottom: 1px solid @slate_border; }
#file-tab, #active-tab, .tab-control { background: transparent; border: 0;
  border-radius: 0; min-width: 0; min-height: 0; padding: 3px 9px; }
#file-tab, #active-tab { border-right: 1px solid @slate_border; }
#file-tab:hover, .tab-control:hover { background: @slate_raised; }
#active-tab { background: @slate_bg; box-shadow: inset 0 -2px @slate_accent; }
.tab-title { color: @slate_text; font-size: 12px; }
.modified-dot { color: @slate_code; font-size: 9px; }
.file-location { color: @slate_muted; font-size: 11px; padding: 0 12px; }
#markdown-modes > * { background: @slate_surface; color: @slate_muted; padding: 1px 9px; min-height: 17px;
  min-width: 0; font-size: 11px; border: 1px solid @slate_border; text-shadow: none; box-shadow: none; }
#markdown-modes > *:hover { background: @slate_raised; color: @slate_text; }
#markdown-modes > *:checked { background: @slate_selection; color: @slate_text; border-color: @slate_border; }
#status-strip { background: @slate_surface; border-top: 1px solid @slate_border; padding: 2px 10px; }
#status-strip label { color: @slate_muted; font-size: 11px; }
#sidebar { background: @slate_surface; color: @slate_text; }
#sidebar .workspace-heading { color: @slate_muted; font-size: 10px; font-weight: bold; letter-spacing: 1.4px; }
#sidebar .workspace-refresh { background: transparent; color: @slate_muted; border: 0; box-shadow: none;
  min-width: 20px; min-height: 20px; padding: 2px; }
#sidebar .workspace-refresh:hover { color: @slate_text; background: @slate_raised; }
#sidebar .workspace-search { background: @slate_bg; color: @slate_text;
  border: 1px solid @slate_border; border-radius: 5px; box-shadow: none; min-height: 27px;
  padding: 1px 7px; font-size: 12px; }
#sidebar .workspace-search:focus { border-color: @slate_accent; }
#sidebar .workspace-mode { background: transparent; color: @slate_muted;
  box-shadow: none; text-shadow: none; border: 1px solid transparent; border-radius: 4px;
  font-size: 11px; min-height: 19px; min-width: 0; padding: 0 7px; }
#sidebar .workspace-mode:hover { color: @slate_text; background: @slate_raised; }
#sidebar .workspace-mode:checked { background: @slate_selection; color: @slate_text; border-color: @slate_border; }
#sidebar .workspace-tree { background: @slate_surface; color: @slate_text; font-size: 12px; }
#sidebar .workspace-tree:selected { background: @slate_selection; color: @slate_text; }
#sidebar .workspace-tree.view { -GtkTreeView-vertical-separator: 7; -GtkTreeView-expander-size: 12; }
#sidebar .workspace-tree:focus { outline-width: 0; }
#sidebar .workspace-note { color: @slate_muted; font-size: 11px; }
)CSS";

std::string stylesheet(const Palette& p) {
    std::string result;
    auto define = [&](const char* name, const std::string& value) {
        result += "@define-color slate_" + std::string(name) + " " + value + ";\n";
    };
    define("bg", p.background);
    define("surface", p.surface);
    define("raised", p.raised);
    define("text", p.text);
    define("muted", p.muted);
    define("border", p.border);
    define("accent", p.accent);
    define("selection", p.selection);
    define("code", p.code_text);
    define("line", p.current_line);
    return result + css;
}

struct Controls {
    Settings& pending;
    std::function<void()> changed;
    std::array<GtkWidget*, fields.size()> swatches{};

    void refresh() {
        for (size_t i = 0; i < fields.size(); ++i) {
            auto color = rgba(pending.colors().*(fields[i].value));
            gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(swatches[i]), &color);
        }
        changed();
    }
};

void add_row(GtkGrid* grid, int row, const char* title, GtkWidget* widget) {
    auto* label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_grid_attach(grid, label, 0, row, 1, 1);
    gtk_grid_attach(grid, widget, 1, row, 1, 1);
    atk_object_set_name(gtk_widget_get_accessible(widget), title);
}

} // namespace

Palette default_palette(bool dark) { return palette_for(dark, defaults(dark)); }

struct Appearance::Impl {
    Settings settings = load();
    Palette current = palette_for(settings.dark, settings.colors());
    GtkCssProvider* css_provider = gtk_css_provider_new();

    void apply(const Palette& next) {
        current = next;
        auto* gtk_settings = gtk_settings_get_default();
        gboolean dark = FALSE;
        g_object_get(gtk_settings, "gtk-application-prefer-dark-theme", &dark, nullptr);
        if (static_cast<bool>(dark) != next.dark)
            g_object_set(gtk_settings, "gtk-application-prefer-dark-theme", next.dark, nullptr);
        gtk_css_provider_load_from_data(css_provider, stylesheet(next).c_str(), -1, nullptr);
    }

    Impl() {
        apply(current);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css_provider),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    ~Impl() {
        gtk_style_context_remove_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css_provider));
        g_object_unref(css_provider);
    }
};

Appearance::Appearance() : impl_(std::make_unique<Impl>()) {}
Appearance::~Appearance() = default;
const Palette& Appearance::palette() const { return impl_->current; }

void Appearance::edit(GtkWindow* parent, const std::function<void(const Palette&)>& changed) {
    auto* dialog = gtk_dialog_new_with_buttons("Appearance", parent,
        GtkDialogFlags(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    auto pending = impl_->settings;
    auto apply = [&](Settings& settings) {
        auto next = palette_for(settings.dark, settings.colors());
        if (next != impl_->current) {
            impl_->apply(next);
            changed(next);
        }
    };
    Controls controls{pending, [&] { apply(pending); }};
    auto* grid = GTK_GRID(gtk_grid_new());
    gtk_grid_set_column_spacing(grid, 24);
    gtk_grid_set_row_spacing(grid, 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                       GTK_WIDGET(grid), TRUE, TRUE, 0);
    auto* mode = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mode), "dark", "Dark");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mode), "light", "Light");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(mode), pending.dark ? "dark" : "light");
    add_row(grid, 0, "Theme", mode);
    g_signal_connect(mode, "changed", G_CALLBACK(+[](GtkComboBox* combo, gpointer data) {
        auto& c = *static_cast<Controls*>(data);
        c.pending.dark = g_strcmp0(gtk_combo_box_get_active_id(combo), "dark") == 0;
        c.refresh();
    }), &controls);
    for (size_t i = 0; i < fields.size(); ++i) {
        auto color = rgba(pending.colors().*(fields[i].value));
        auto* swatch = gtk_color_button_new_with_rgba(&color);
        controls.swatches[i] = swatch;
        gtk_color_button_set_title(GTK_COLOR_BUTTON(swatch), fields[i].label);
        add_row(grid, static_cast<int>(i + 1), fields[i].label, swatch);
        g_signal_connect(swatch, "color-set", G_CALLBACK(+[](GtkColorButton* button, gpointer data) {
            auto& c = *static_cast<Controls*>(data);
            for (size_t i = 0; i < fields.size(); ++i) {
                if (c.swatches[i] == GTK_WIDGET(button)) {
                    GdkRGBA color;
                    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &color);
                    c.pending.colors().*(fields[i].value) = hex(color);
                    c.changed();
                    break;
                }
            }
        }), &controls);
    }
    auto* reset = gtk_button_new_with_label("Restore theme defaults");
    gtk_widget_set_halign(reset, GTK_ALIGN_START);
    gtk_grid_attach(grid, reset, 0, static_cast<int>(fields.size() + 1), 2, 1);
    g_signal_connect(reset, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& c = *static_cast<Controls*>(data);
        c.pending.colors() = defaults(c.pending.dark);
        c.refresh();
    }), &controls);
    gtk_widget_show_all(dialog);
    const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response == GTK_RESPONSE_ACCEPT) {
        try {
            save(pending);
            impl_->settings = std::move(pending);
        } catch (...) {
            apply(impl_->settings);
            throw;
        }
    } else {
        apply(impl_->settings);
    }
}

} // namespace slate
