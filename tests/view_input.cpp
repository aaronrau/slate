#include "document.hpp"
#include "editor_view.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {

void check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
void flush() {
    while (gtk_events_pending()) gtk_main_iteration();
}
void key(GtkWidget* area, guint keyval, GdkModifierType modifiers = GdkModifierType(0)) {
    auto event = gdk_event_new(GDK_KEY_PRESS);
    event->key.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(area)));
    event->key.keyval = keyval;
    event->key.state = modifiers;
    event->key.string = g_strdup("");
    gboolean handled = FALSE;
    g_signal_emit_by_name(area, "key-press-event", &event->key, &handled);
    gdk_event_free(event);
    check(handled, gdk_keyval_name(keyval));
    flush();
}
void mouse(GtkWidget* area, GdkEventType type, double x, guint button = 1) {
    auto event = gdk_event_new(type);
    auto seat = gdk_display_get_default_seat(gtk_widget_get_display(area));
    gdk_event_set_device(event, gdk_seat_get_pointer(seat));
    if (type == GDK_MOTION_NOTIFY) {
        event->motion.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(area)));
        event->motion.x = x;
        event->motion.y = 15;
    } else {
        event->button.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(area)));
        event->button.button = button;
        event->button.x = x;
        event->button.y = 15;
    }
    gtk_widget_event(area, event);
    gdk_event_free(event);
    flush();
}

void exercise() {
    slate::Document doc;
    doc.replace(0, 0, "é🪶X\r\n12345\r\nabc\r\n");
    auto window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 700, 400);
    slate::EditorView edit(doc);
    gtk_container_add(GTK_CONTAINER(window), edit.widget());
    gtk_widget_show_all(window);
    edit.focus();
    flush();
    auto area = gtk_grid_get_child_at(GTK_GRID(edit.widget()), 0, 0);
    key(area, GDK_KEY_Right);
    check(edit.cursor() == 2, "two-byte code point");
    key(area, GDK_KEY_Right);
    check(edit.cursor() == 6, "four-byte code point");
    key(area, GDK_KEY_End);
    check(edit.cursor() == 7, "line end");
    key(area, GDK_KEY_Right);
    check(edit.cursor() == 9, "CRLF as one line break");
    key(area, GDK_KEY_Down);
    check(edit.cursor() == 16, "vertical movement down");
    key(area, GDK_KEY_Up);
    check(edit.cursor() == 9, "vertical movement up");
    key(area, GDK_KEY_BackSpace);
    check(doc.read(0, 17) == "é🪶X12345\r\nabc", "backspace over CRLF");
    edit.undo();
    check(doc.read(0, 9) == "é🪶X\r\n", "undo restores CRLF");
    edit.jump(9, 5);
    edit.cut();
    check(edit.cursor() == 9 && doc.read(9, 2) == "\r\n", "cut selection");
    edit.paste();
    flush();
    check(doc.read(9, 5) == "12345", "paste clipboard");
    auto primary = gtk_widget_get_modifier_mask(area, GDK_MODIFIER_INTENT_PRIMARY_ACCELERATOR);
    key(area, GDK_KEY_End, primary);
    key(area, GDK_KEY_Return);
    check(doc.read(doc.size() - 2, 2) == "\r\n", "new line uses CRLF");

    char filename[] = "/tmp/slate-view-save-XXXXXX";
    int fd = mkstemp(filename);
    check(fd >= 0, "create save target");
    close(fd);
    auto saved = doc.snapshot();
    saved.save(filename);
    doc = std::move(saved);
    std::filesystem::remove(filename);
    edit.undo();
    check(doc.path() == filename && doc.read(doc.size() - 5, 5) == "abc\r\n", "undo after Save As");

    doc = slate::Document();
    doc.replace(0, 0, "abcd\n12345\n");
    edit.reset();
    // Drag back from the empty space after the first line to its text margin.
    mouse(area, GDK_BUTTON_PRESS, 600);
    mouse(area, GDK_MOTION_NOTIFY, 60);
    mouse(area, GDK_BUTTON_RELEASE, 60);
    check(edit.selection_size() == 4, "drag selects first line text");
    mouse(area, GDK_BUTTON_PRESS, 78, 3);
    check(edit.selection_size() == 4, "right-click preserves selection");
    auto menus = gtk_menu_get_for_attach_widget(area);
    check(menus != nullptr, "editor context menu");
    auto menu = GTK_MENU(menus->data);
    auto items = gtk_container_get_children(GTK_CONTAINER(menu));
    GtkWidget* cut_item = nullptr;
    for (auto it = items; it; it = it->next) {
        auto item = GTK_MENU_ITEM(it->data);
        if (g_strcmp0(gtk_menu_item_get_label(item), "Cut") == 0) cut_item = GTK_WIDGET(item);
    }
    g_list_free(items);
    check(cut_item && gtk_widget_is_sensitive(cut_item), "Cut available for selection");
    gtk_menu_item_activate(GTK_MENU_ITEM(cut_item));
    gtk_menu_popdown(menu);
    check(doc.read(0, 2) == "\n1", "cut from the native context menu");
    edit.undo();
    key(area, GDK_KEY_Delete);
    check(doc.read(0, 2) == "\n1", "delete mouse selection");
    doc = slate::Document();
    doc.replace(0, 0, std::string(9000, 'x'));
    edit.reset();
    key(area, GDK_KEY_End);
    auto segment_end = edit.cursor();
    check(segment_end > 0 && segment_end < doc.size(), "bounded long-line segment");
    key(area, GDK_KEY_End);
    check(edit.cursor() == segment_end, "End stays at the selected segment end");
    key(area, GDK_KEY_Home);
    check(edit.cursor() == 0, "Home returns to the beginning of that segment");
    gtk_widget_destroy(window);
    flush();
}

} // namespace

int main(int argc, char** argv) {
    if (!gtk_init_check(&argc, &argv)) return 77;
    try { exercise(); }
    catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
