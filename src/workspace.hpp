#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace slate {

class Workspace {
public:
    explicit Workspace(const std::filesystem::path& root);
    ~Workspace();
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    GtkWidget* widget() const;
    void set_root(const std::filesystem::path& root);
    void focus_search(bool content);

    std::function<void(const std::filesystem::path&, std::uint64_t, std::size_t)> on_open;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace slate
