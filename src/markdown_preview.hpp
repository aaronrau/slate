#pragma once

#include "document.hpp"
#include <gtk/gtk.h>
#include <functional>

namespace slate {

class MarkdownPreview {
public:
    MarkdownPreview();
    ~MarkdownPreview();
    MarkdownPreview(const MarkdownPreview&) = delete;
    MarkdownPreview& operator=(const MarkdownPreview&) = delete;
    GtkWidget* widget() const;
    void show(Document snapshot, std::uint64_t offset = 0, std::string fragment = {});
    void clear();
    void focus();
    void set_open_link(std::function<void(const std::filesystem::path&, const std::string&)> open);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slate
