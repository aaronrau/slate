#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <gtk/gtk.h>

namespace slate {

class Document;
struct Palette;

class EditorView {
public:
    explicit EditorView(Document& document);
    ~EditorView();
    EditorView(const EditorView&) = delete;
    EditorView& operator=(const EditorView&) = delete;

    GtkWidget* widget() const;
    void set_palette(const Palette& palette);
    void focus();
    void reset();
    void jump(std::uint64_t byte_offset, std::size_t select_bytes = 0);
    std::uint64_t cursor() const;
    std::uint64_t selection_size() const;
    void undo();
    void redo();
    void cut();
    void copy();
    void paste();
    void select_all();

    std::function<void()> on_change;
    std::function<void(const std::string&)> on_error;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace slate
