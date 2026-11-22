#include "document.hpp"
#include "editor_view.hpp"
#include "file_history.hpp"
#include "markdown_preview.hpp"
#include "theme.hpp"
#include "workspace.hpp"

#include <gtk/gtk.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace slate {
namespace {
using Action = std::function<void()>;

void style(GtkWidget* widget, const char* name) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), name);
}

GtkWidget* label(const char* text, const char* css = nullptr) {
    auto* result = gtk_label_new(text);
    if (css) style(result, css);
    return result;
}

void connect_action(GtkWidget* widget, const char* signal, Action action) {
    g_signal_connect_data(widget, signal, G_CALLBACK(+[](GtkWidget*, gpointer data) {
        (*static_cast<Action*>(data))();
    }), new Action(std::move(action)), +[](gpointer data, GClosure*) {
        delete static_cast<Action*>(data);
    }, GConnectFlags(0));
}

std::string display_name(const fs::path& path) {
    gchar* display = g_filename_display_name(path.c_str());
    std::string name(display);
    g_free(display);
    return name;
}

std::string format_size(uint64_t bytes) {
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    double n = static_cast<double>(bytes);
    int unit = 0;
    while (n >= 1024 && unit < 6) { n /= 1024; ++unit; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), unit ? "%.1f %s" : "%.0f %s", n, units[unit]);
    return buf;
}

bool markdown(const fs::path& path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), g_ascii_tolower);
    return ext == ".md" || ext == ".markdown";
}

fs::path history_database(const fs::path& workspace) {
    fs::path directory = fs::path(g_get_user_data_dir()) / "slate" / "history";
    if (g_mkdir_with_parents(directory.c_str(), 0700) != 0) return {};
    auto* digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, workspace.c_str(), -1);
    auto database = directory / (std::string(digest) + ".sqlite3");
    g_free(digest);
    return database;
}

class Window {
    enum class Edit { undo, redo, cut, copy, paste, select_all, count };
    fs::path root_;
    FileHistory history_;
    Appearance appearance_;
    Document document_;
    EditorView editor_;
    Workspace workspace_;
    std::optional<MarkdownPreview> preview_;
    GtkWidget* win_ = nullptr;
    GtkWidget* stack_ = nullptr;
    GtkWidget* modes_ = nullptr;
    GtkWidget* source_button_ = nullptr;
    GtkWidget* preview_button_ = nullptr;
    GtkWidget* preview_item_ = nullptr;
    GtkWidget* sidebar_item_ = nullptr;
    std::array<GtkWidget*, static_cast<size_t>(Edit::count)> edit_items_{};
    struct TabButton {
        GtkWidget* button = nullptr;
        GtkWidget* title = nullptr;
        GtkWidget* dot = nullptr;
    };
    std::array<TabButton, 5> tabs_;
    std::vector<HistoryFile> visible_tabs_;
    GtkWidget* back_ = nullptr;
    GtkWidget* forward_ = nullptr;
    GtkWidget* back_item_ = nullptr;
    GtkWidget* forward_item_ = nullptr;
    GtkWidget* location_ = nullptr;
    GtkWidget* syntax_ = nullptr;
    GtkWidget* position_ = nullptr;
    GtkWidget* state_ = nullptr;
    GtkWidget* dot_ = nullptr;
    GtkAccelGroup* shortcuts_ = nullptr;
    bool showing_error_ = false;
    bool saving_ = false;
    bool close_requested_ = false;
    std::future<Document> save_job_;
    Action after_save_;
    guint save_poll_ = 0;

    void error(const std::string& message) {
        if (showing_error_) return;
        showing_error_ = true;
        auto* dialog = gtk_message_dialog_new(GTK_WINDOW(win_), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", message.c_str());
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        showing_error_ = false;
    }

    // All actions reach GTK through this exception boundary.
    void run(const Action& action) {
        try { action(); }
        catch (const std::exception& e) { error(e.what()); }
    }

    std::optional<fs::path> choose(GtkFileChooserAction action) {
        const bool save = action == GTK_FILE_CHOOSER_ACTION_SAVE;
        const char* title = save ? "Save As" :
            (action == GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER ? "Open Folder" : "Open File");
        auto* dialog = gtk_file_chooser_dialog_new(title, GTK_WINDOW(win_), action,
            "_Cancel", GTK_RESPONSE_CANCEL, save ? "_Save" : "_Open", GTK_RESPONSE_ACCEPT, nullptr);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
        auto* chooser = GTK_FILE_CHOOSER(dialog);
        gtk_file_chooser_set_local_only(chooser, TRUE);
        gtk_file_chooser_set_current_folder(chooser, root_.c_str());
        if (save) {
            gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);
            auto current = document_.path();
            if (current.empty())
                gtk_file_chooser_set_current_name(chooser, "untitled.txt");
            else
                gtk_file_chooser_set_filename(chooser, current.c_str());
        }
        std::optional<fs::path> selected;
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            gchar* filename = gtk_file_chooser_get_filename(chooser);
            if (filename) selected = fs::path(filename);
            g_free(filename);
        }
        gtk_widget_destroy(dialog);
        return selected;
    }

    void update_status() {
        if (!win_) return;
        const bool changed = document_.dirty();
        gtk_label_set_text(GTK_LABEL(state_), saving_ ? "Saving…" :
            changed ? "Modified" : (document_.path().empty() ? "New document" : "Saved"));
        if (dot_) gtk_widget_set_visible(dot_, changed);
        std::string info;
        if (!in_preview()) {
            info = "Byte " + std::to_string(editor_.cursor()) + "   ·   ";
            if (auto count = editor_.selection_size())
                info += format_size(count) + " selected   ·   ";
        }
        info += format_size(document_.size());
        gtk_label_set_text(GTK_LABEL(position_), info.c_str());
    }

    void update_file() {
        auto file = document_.path();
        auto name = file.empty() ? std::string("Untitled") : display_name(file.filename());
        visible_tabs_ = history_.nearby_tabs();
        dot_ = nullptr;
        for (size_t i = 0; i < tabs_.size(); ++i) {
            auto& tab = tabs_[i];
            bool visible = i < visible_tabs_.size();
            gtk_widget_set_visible(tab.button, visible);
            if (!visible) continue;
            const auto& path = visible_tabs_[i].path;
            bool active = file == path;
            auto title = path.empty() ? std::string("Untitled") : display_name(path.filename());
            gtk_label_set_text(GTK_LABEL(tab.title), title.c_str());
            gtk_widget_set_tooltip_text(tab.button, path.empty() ? "Untitled" : display_name(path).c_str());
            gtk_widget_set_name(tab.button, active ? "active-tab" : "file-tab");
            gtk_widget_set_visible(tab.dot, active && document_.dirty());
            if (active) dot_ = tab.dot;
        }
        bool back = history_.previous().has_value(), forward = history_.next().has_value();
        for (auto* widget : {back_, back_item_}) gtk_widget_set_sensitive(widget, back);
        for (auto* widget : {forward_, forward_item_}) gtk_widget_set_sensitive(widget, forward);
        fs::path parent = file.empty() ? fs::path{} : file.parent_path().lexically_relative(root_);
        gtk_label_set_text(GTK_LABEL(location_), parent == "." ? "" : display_name(parent).c_str());
        bool md = markdown(file);
        gtk_label_set_text(GTK_LABEL(syntax_), md ? "Markdown" : "Plain text");
        gtk_widget_set_visible(modes_, md);
        gtk_widget_set_sensitive(preview_item_, md);
        auto root_name = display_name(root_.filename().empty() ? root_ : root_.filename());
        gtk_window_set_title(GTK_WINDOW(win_), (name + " — " + root_name + " — Slate").c_str());
        update_status();
    }

    bool in_preview() const {
        return preview_ && gtk_stack_get_visible_child(GTK_STACK(stack_)) == preview_->widget();
    }

    void focus_document() {
        if (in_preview()) preview_->focus();
        else editor_.focus();
    }

    void set_view(bool rendered, bool refresh = false, const std::string& fragment = {}) {
        if (rendered) {
            if (!preview_) {
                preview_.emplace();
                preview_->set_palette(appearance_.palette());
                preview_->set_open_link([this](const fs::path& path, const std::string& anchor) {
                    run([&] { open_file(path, 0, 0, false, anchor); });
                });
                gtk_stack_add_named(GTK_STACK(stack_), preview_->widget(), "preview");
                gtk_widget_show_all(preview_->widget());
            }
            if (!in_preview() || refresh || !fragment.empty())
                preview_->show(document_.snapshot(), editor_.cursor(), fragment);
            gtk_stack_set_visible_child(GTK_STACK(stack_), preview_->widget());
        } else {
            gtk_stack_set_visible_child(GTK_STACK(stack_), editor_.widget());
            if (preview_) preview_->clear();
        }
        gtk_widget_set_sensitive(editor_.widget(), !rendered && !saving_);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rendered ? preview_button_ : source_button_), TRUE);
        update_status();
        focus_document();
    }

    bool finish_save() {
        if (save_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return false;
        save_poll_ = 0;
        auto next = std::move(after_save_);
        saving_ = false;
        gtk_widget_set_sensitive(editor_.widget(), !in_preview());
        try {
            const auto previous_path = document_.path();
            document_ = save_job_.get();
            gtk_widget_queue_draw(editor_.widget());
            if (previous_path != document_.path()) {
                history_.saved_as(document_.path());
                const bool is_markdown = markdown(document_.path());
                if (in_preview() || markdown(previous_path) != is_markdown)
                    set_view(is_markdown, true);
            }
            update_file();
        } catch (const std::exception& e) {
            close_requested_ = false;
            update_status();
            error(e.what());
            return true;
        }
        if (close_requested_) { close_requested_ = false; request_close(); }
        else if (next) run(next);
        else focus_document();
        return true;
    }

    void save(bool save_as, Action next = {}) {
        if (saving_) return;
        auto path = document_.path();
        if (save_as || path.empty()) {
            auto selected = choose(GTK_FILE_CHOOSER_ACTION_SAVE);
            if (!selected) return;
            path = *selected;
        }
        save_job_ = std::async(std::launch::async,
            [snapshot = document_.snapshot(), path]() mutable {
                snapshot.save(path);
                return std::move(snapshot);
            });
        after_save_ = std::move(next);
        saving_ = true;
        gtk_widget_set_sensitive(editor_.widget(), FALSE);
        update_status();
        save_poll_ = g_timeout_add(25, +[](gpointer data) -> gboolean {
            return static_cast<Window*>(data)->finish_save() ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
        }, this);
    }

    void with_clean_document(Action next) {
        if (saving_) return;
        if (!document_.dirty()) { next(); return; }
        auto path = document_.path();
        auto name = path.empty() ? std::string("Untitled") : display_name(path.filename());
        auto* dialog = gtk_message_dialog_new(GTK_WINDOW(win_), GTK_DIALOG_MODAL,
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "Save changes to “%s”?", name.c_str());
        gtk_dialog_add_buttons(GTK_DIALOG(dialog), "_Discard", GTK_RESPONSE_REJECT,
            "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
        int response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response == GTK_RESPONSE_ACCEPT) save(false, std::move(next));
        else if (response == GTK_RESPONSE_REJECT) next();
    }

    void open_file(const fs::path& path, uint64_t offset = 0, size_t bytes = 0, bool reload = false,
                   const std::string& fragment = {}, std::int64_t visit = 0, bool close_tab = false) {
        if (saving_) return;
        if (!reload && path == document_.path()) {
            if (offset || bytes) editor_.jump(offset, bytes);
            if (bytes || !fragment.empty()) set_view(!bytes && markdown(path), false, fragment);
            else focus_document();
            return;
        }
        with_clean_document([this, path, offset, bytes, fragment, visit, close_tab] {
            Document opened;
            if (!path.empty()) opened.open(path);
            auto remembered = history_.position(path);
            history_.remember(document_.path(), editor_.cursor(), in_preview());
            history_.visit(path, visit, close_tab);
            document_ = std::move(opened);
            editor_.reset();
            auto cursor = (bytes || offset || !fragment.empty() || !remembered) ? offset : remembered->cursor;
            if (cursor || bytes) editor_.jump(cursor, bytes);
            update_file();
            bool preview = markdown(path) && !bytes && (!fragment.empty() || !remembered ||
                                                        remembered->preview.value_or(true));
            set_view(preview, true, fragment);
        });
    }

    void navigate(bool forward) {
        if (auto dest = forward ? history_.next() : history_.previous())
            open_file(dest->path, 0, 0, true, {}, dest->visit);
    }

    std::optional<fs::path> adjacent_tab(int step) const {
        for (int i = 0; i < static_cast<int>(visible_tabs_.size()); ++i) {
            const int target = i + step;
            if (visible_tabs_[i].path == document_.path() &&
                target >= 0 && target < static_cast<int>(visible_tabs_.size()))
                return visible_tabs_[target].path;
        }
        return {};
    }

    void close_tab() {
        auto dest = adjacent_tab(-1);
        if (!dest) dest = adjacent_tab(1);
        if (!dest && document_.path().empty()) { request_close(); return; }
        open_file(dest.value_or(fs::path{}), 0, 0, true, {}, 0, true);
    }

    void open_dialog() {
        if (saving_) return;
        if (auto file = choose(GTK_FILE_CHOOSER_ACTION_OPEN)) open_file(*file, 0, 0, true);
    }

    void request_close() {
        if (saving_) { close_requested_ = true; return; }
        with_clean_document([this] {
            history_.remember(document_.path(), editor_.cursor(), in_preview());
            gtk_main_quit();
        });
    }

    void edit(Edit command) {
        auto* focus = gtk_window_get_focus(GTK_WINDOW(win_));
        if (!focus) return;
        if (gtk_widget_is_ancestor(focus, editor_.widget())) {
            switch (command) {
            case Edit::undo: editor_.undo(); break;
            case Edit::redo: editor_.redo(); break;
            case Edit::cut: editor_.cut(); break;
            case Edit::copy: editor_.copy(); break;
            case Edit::paste: editor_.paste(); break;
            case Edit::select_all: editor_.select_all(); break;
            case Edit::count: break;
            }
        } else if (GTK_IS_EDITABLE(focus)) {
            auto* field = GTK_EDITABLE(focus);
            switch (command) {
            case Edit::cut: gtk_editable_cut_clipboard(field); break;
            case Edit::copy: gtk_editable_copy_clipboard(field); break;
            case Edit::paste: gtk_editable_paste_clipboard(field); break;
            case Edit::select_all: gtk_editable_select_region(field, 0, -1); break;
            default: break;
            }
        } else if (GTK_IS_TEXT_VIEW(focus)) {
            if (command == Edit::copy) g_signal_emit_by_name(focus, "copy-clipboard");
            if (command == Edit::select_all) g_signal_emit_by_name(focus, "select-all", TRUE);
        }
    }

    void update_edit_items(GtkWidget* focus) {
        bool source = focus && gtk_widget_is_ancestor(focus, editor_.widget()) && !saving_;
        bool entry = focus && GTK_IS_EDITABLE(focus);
        bool readable = source || entry || (focus && GTK_IS_TEXT_VIEW(focus));
        bool writable = source || (entry && gtk_editable_get_editable(GTK_EDITABLE(focus)));
        auto enable = [&](Edit which, bool enabled) {
            gtk_widget_set_sensitive(edit_items_[static_cast<size_t>(which)], enabled);
        };
        enable(Edit::undo, source);
        enable(Edit::redo, source);
        enable(Edit::cut, writable);
        enable(Edit::paste, writable);
        enable(Edit::copy, readable);
        enable(Edit::select_all, readable);
    }

    GtkWidget* submenu(GtkWidget* bar, const char* name) {
        auto* title = gtk_menu_item_new_with_mnemonic(name);
        auto* menu = gtk_menu_new();
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(title), menu);
        gtk_menu_shell_append(GTK_MENU_SHELL(bar), title);
        return menu;
    }

    GtkWidget* menu_item(GtkWidget* menu, const char* name, const char* shortcut, Action action) {
        auto* item = gtk_menu_item_new_with_mnemonic(name);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        connect_action(item, "activate", [this, action = std::move(action)] { run(action); });
        if (shortcut) {
            guint key;
            GdkModifierType mods;
            gtk_accelerator_parse(shortcut, &key, &mods);
            gtk_widget_add_accelerator(item, "activate", shortcuts_, key, mods, GTK_ACCEL_VISIBLE);
        }
        return item;
    }

    void edit_item(GtkWidget* menu, const char* name, const char* shortcut, Edit command) {
        edit_items_[static_cast<size_t>(command)] =
            menu_item(menu, name, shortcut, [this, command] { edit(command); });
    }

    void find(bool content) {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(sidebar_item_), TRUE);
        workspace_.focus_search(content);
    }

    GtkWidget* build_menu() {
        auto* bar = gtk_menu_bar_new();
        gtk_widget_set_name(bar, "main-menu");
        shortcuts_ = gtk_accel_group_new();
        gtk_window_add_accel_group(GTK_WINDOW(win_), shortcuts_);
        g_object_unref(shortcuts_);

        auto* file = submenu(bar, "_File");
        menu_item(file, "_New File", "<Primary>n", [this] {
            open_file({}, 0, 0, true);
        });
        menu_item(file, "_Open File…", "<Primary>o", [this] { open_dialog(); });
        menu_item(file, "Open _Folder…", "<Primary><Shift>o", [this] {
            if (auto dir = choose(GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER)) {
                workspace_.set_root(*dir);
                root_ = *dir;
                update_file();
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(sidebar_item_), TRUE);
            }
        });
        gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
        menu_item(file, "_Save", "<Primary>s", [this] { save(false); });
        menu_item(file, "Save _As…", "<Primary><Shift>s", [this] { save(true); });
        gtk_menu_shell_append(GTK_MENU_SHELL(file), gtk_separator_menu_item_new());
        menu_item(file, "Close _Tab", "<Primary>F4", [this] { close_tab(); });
        menu_item(file, "_Close Window", "<Primary>w", [this] { request_close(); });

        auto* editing = submenu(bar, "_Edit");
        edit_item(editing, "_Undo", "<Primary>z", Edit::undo);
        edit_item(editing, "_Redo", "<Primary><Shift>z", Edit::redo);
        gtk_menu_shell_append(GTK_MENU_SHELL(editing), gtk_separator_menu_item_new());
        edit_item(editing, "Cu_t", "<Primary>x", Edit::cut);
        edit_item(editing, "_Copy", "<Primary>c", Edit::copy);
        edit_item(editing, "_Paste", "<Primary>v", Edit::paste);

        gtk_menu_shell_append(GTK_MENU_SHELL(editing), gtk_separator_menu_item_new());
        menu_item(editing, "_Appearance…", "<Primary>comma", [this] {
            appearance_.edit(GTK_WINDOW(win_), [this](const Palette& colors) {
                editor_.set_palette(colors);
                if (preview_) preview_->set_palette(colors);
            });
        });

        auto* selection = submenu(bar, "_Selection");
        edit_item(selection, "Select _All", "<Primary>a", Edit::select_all);

        auto* search = submenu(bar, "F_ind");
        menu_item(search, "Find in _Files…", "<Primary><Shift>f", [this] { find(true); });

        auto* view = submenu(bar, "_View");
        sidebar_item_ = gtk_check_menu_item_new_with_mnemonic("Show _Side Bar");
        gtk_menu_shell_append(GTK_MENU_SHELL(view), sidebar_item_);
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(sidebar_item_), TRUE);
        connect_action(sidebar_item_, "toggled", [this] {
            bool show = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(sidebar_item_));
            gtk_widget_set_visible(workspace_.widget(), show);
            if (!show) focus_document();
        });
        preview_item_ = menu_item(view, "Toggle Markdown _Preview", "<Primary><Shift>m",
                                  [this] { set_view(!in_preview()); });

        auto* go = submenu(bar, "_Goto");
        back_item_ = menu_item(go, "_Back", "<Alt>Left", [this] { navigate(false); });
        forward_item_ = menu_item(go, "Fo_rward", "<Alt>Right", [this] { navigate(true); });
        menu_item(go, "Previous _Tab", "<Primary>Page_Up", [this] {
            if (auto file = adjacent_tab(-1)) open_file(*file);
        });
        menu_item(go, "_Next Tab", "<Primary>Page_Down", [this] {
            if (auto file = adjacent_tab(1)) open_file(*file);
        });
        menu_item(go, "Goto _File…", "<Primary>p", [this] { find(false); });
        return bar;
    }

    GtkWidget* build_editor() {
        auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        auto* strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_name(strip, "tab-strip");
        gtk_widget_set_size_request(strip, -1, 31);
        auto add_control = [&](const char* icon, const char* hint, Action action) {
            auto* button = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
            style(button, "tab-control");
            gtk_widget_set_tooltip_text(button, hint);
            atk_object_set_name(gtk_widget_get_accessible(button), hint);
            gtk_box_pack_start(GTK_BOX(strip), button, FALSE, FALSE, 0);
            connect_action(button, "clicked", [this, action = std::move(action)] { run(action); });
            return button;
        };
        back_ = add_control("go-previous-symbolic", "Back (Alt+Left)", [this] { navigate(false); });
        forward_ = add_control("go-next-symbolic", "Forward (Alt+Right)", [this] { navigate(true); });
        for (size_t i = 0; i < tabs_.size(); ++i) {
            auto& tab = tabs_[i];
            tab.button = gtk_button_new();
            auto* contents = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
            tab.title = label("", "tab-title");
            gtk_label_set_ellipsize(GTK_LABEL(tab.title), PANGO_ELLIPSIZE_MIDDLE);
            gtk_label_set_max_width_chars(GTK_LABEL(tab.title), 18);
            tab.dot = label("●", "modified-dot");
            gtk_box_pack_start(GTK_BOX(contents), tab.title, TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(contents), tab.dot, FALSE, FALSE, 0);
            gtk_container_add(GTK_CONTAINER(tab.button), contents);
            gtk_box_pack_start(GTK_BOX(strip), tab.button, FALSE, FALSE, 0);
            connect_action(tab.button, "clicked", [this, i] {
                // open_file can resize visible_tabs_ while this signal runs.
                auto file = visible_tabs_.at(i).path;
                run([&] { open_file(file); });
            });
        }
        add_control("window-close-symbolic", "Close current tab", [this] { close_tab(); });
        location_ = label("", "file-location");
        gtk_label_set_ellipsize(GTK_LABEL(location_), PANGO_ELLIPSIZE_START);
        gtk_label_set_xalign(GTK_LABEL(location_), 0);
        gtk_box_pack_start(GTK_BOX(strip), location_, TRUE, TRUE, 0);

        modes_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_name(modes_, "markdown-modes");
        style(modes_, GTK_STYLE_CLASS_LINKED);
        gtk_widget_set_valign(modes_, GTK_ALIGN_CENTER);
        source_button_ = gtk_radio_button_new_with_label(nullptr, "Source");
        preview_button_ = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(source_button_),
                                                                      "Preview");
        gtk_widget_set_tooltip_text(source_button_, "Edit Markdown source");
        gtk_widget_set_tooltip_text(preview_button_, "Read the rendered document");
        for (auto* button : {source_button_, preview_button_}) {
            gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(button), FALSE);
            gtk_box_pack_start(GTK_BOX(modes_), button, FALSE, FALSE, 0);
            connect_action(button, "toggled", [this, button] {
                bool rendered = button == preview_button_;
                if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)) && rendered != in_preview())
                    run([&] { set_view(rendered); });
            });
        }
        gtk_box_pack_end(GTK_BOX(strip), modes_, FALSE, FALSE, 8);
        gtk_box_pack_start(GTK_BOX(box), strip, FALSE, FALSE, 0);
        stack_ = gtk_stack_new();
        gtk_stack_set_homogeneous(GTK_STACK(stack_), FALSE);
        gtk_stack_add_named(GTK_STACK(stack_), editor_.widget(), "source");
        gtk_box_pack_start(GTK_BOX(box), stack_, TRUE, TRUE, 0);
        return box;
    }

    GtkWidget* build_status() {
        auto* strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_name(strip, "status-strip");
        state_ = label("");
        gtk_box_pack_start(GTK_BOX(strip), state_, FALSE, FALSE, 0);
        syntax_ = label("");
        position_ = label("");
        gtk_box_pack_end(GTK_BOX(strip), syntax_, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(strip), label("·"), FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(strip), position_, FALSE, FALSE, 0);
        return strip;
    }

public:
    explicit Window(const fs::path& root)
        : root_(root), history_(history_database(root)), editor_(document_), workspace_(root) {
        editor_.set_palette(appearance_.palette());
        win_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_widget_set_name(win_, "slate");
        gtk_window_set_default_size(GTK_WINDOW(win_), 1120, 720);
        auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_pack_start(GTK_BOX(box), build_menu(), FALSE, FALSE, 0);
        auto* split = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_paned_pack1(GTK_PANED(split), workspace_.widget(), FALSE, FALSE);
        gtk_paned_pack2(GTK_PANED(split), build_editor(), TRUE, TRUE);
        gtk_paned_set_position(GTK_PANED(split), 230);
        gtk_box_pack_start(GTK_BOX(box), split, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(box), build_status(), FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(win_), box);
        gtk_widget_show_all(box);

        editor_.on_change = [this] { update_status(); };
        editor_.on_error = [this](const std::string& message) { error(message); };
        workspace_.on_open = [this](const fs::path& path, uint64_t pos, size_t bytes) {
            run([&] { open_file(path, pos, bytes); });
        };
        g_signal_connect_after(win_, "set-focus", G_CALLBACK(+[](GtkWindow*, GtkWidget* focus,
                                                                 gpointer data) {
            static_cast<Window*>(data)->update_edit_items(focus);
        }), this);
        g_signal_connect(win_, "delete-event",
            G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
                auto* self = static_cast<Window*>(data);
                self->run([self] { self->request_close(); });
                return TRUE;
            }), this);
        update_file();
    }

    void show(const std::optional<fs::path>& file) {
        std::optional<HistoryFile> saved = history_.current();
        if (file) open_file(*file, 0, 0, true);
        else if (saved && (saved->path.empty() || fs::is_regular_file(saved->path)))
            open_file(saved->path, 0, 0, true, {}, saved->visit);
        else open_file({}, 0, 0, true);
        gtk_widget_show(win_);
        focus_document();
    }

    ~Window() {
        if (save_poll_) g_source_remove(save_poll_);
        editor_.on_change = {}; editor_.on_error = {}; workspace_.on_open = {};
        g_signal_handlers_disconnect_by_data(win_, this);
        gtk_widget_destroy(win_);
    }
};

}
}

int main(int argc, char** argv) {
    bool help = argc == 2 && std::string_view(argv[1]) == "--help";
    if (argc > 2 || help) {
        std::cout << "Usage: slate [FILE | DIRECTORY]\n";
        return help ? 0 : 2;
    }
    try {
        std::optional<fs::path> file;
        fs::path root = fs::current_path();
        if (argc == 2) {
            auto path = fs::absolute(argv[1]).lexically_normal();
            if (fs::is_directory(path))
                root = path.filename().empty() && path != path.root_path() ? path.parent_path() : path;
            else { file = path; root = path.parent_path(); }
        }
        g_set_prgname("slate");
        g_set_application_name("Slate");
#ifdef __linux__
        // The canvas uses Cairo. Skip loading a GL driver for a window with no GL widgets.
        g_setenv("GDK_GL", "disable", FALSE);
#endif
        if (!gtk_init_check(nullptr, nullptr)) {
            std::cerr << "Slate needs a graphical session to open its window.\n";
            return 1;
        }
        gtk_icon_theme_add_resource_path(gtk_icon_theme_get_default(), "/slate/icons");
        gtk_window_set_default_icon_name("slate-editor");
        slate::Window window(root);
        window.show(file);
        gtk_main();
    } catch (const std::exception& e) {
        std::cerr << "slate: " << e.what() << "\n";
        return 1;
    }
}
