#include "file_history.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void exercise(const fs::path& database) {
    constexpr int visits = 24000;
    const auto target = [](int i) { return fs::path("/project") / std::to_string(i % 97); };
    {
        slate::FileHistory h(database);
        require(!h.current() && !h.previous() && !h.next(), "unexpected initial history");
        for (int i = 0; i < visits; ++i) h.visit(target(i));
        h.remember(target(visits - 1), (1ULL << 34) + 7, false);
        require(h.current()->visit == visits, "lost visits");
        require(h.nearby_tabs().size() <= 5, "loaded too many visible tabs");
        h.visit(target(visits - 1));
        require(h.current()->visit == visits, "duplicate entry");
        slate::FileHistory other_window(database);
        other_window.visit("/other-window");
        require(!other_window.previous(), "simultaneous windows mixed navigation cursors");
    }
    {
        slate::FileHistory h(database);
        require(h.current()->cursor == (1ULL << 34) + 7 && h.current()->preview == false,
                "large cursor or source mode lost across restart");
        for (int i = visits - 1; i > 0; --i) {
            auto back = h.previous();
            require(back && back->path == target(i - 1), "wrong previous file");
            h.visit(back->path, back->visit);
        }
        require(!h.previous() && h.next()->path == target(1), "beginning of history");
        auto forward = h.next();
        h.visit(forward->path, forward->visit);
        require(h.current()->visit == 2, "wrong forward visit");
        h.visit("/branch");
        require(!h.next() && h.previous()->path == target(1), "forward branch survived");
        h.saved_as("/renamed");
        require(h.current()->path == "/renamed" && h.previous()->path == target(1),
                "Save As added a spurious visit");
        h.visit({}, 0, true);
        require(h.current()->path.empty(), "untitled tab");
        for (const auto& t : h.nearby_tabs()) require(t.path != "/renamed", "tab failed to close");
        auto closed = h.previous();
        h.visit(closed->path, closed->visit);
        auto tabs = h.nearby_tabs();
        require(tabs.back().path == "/renamed", "history failed to reopen a closed tab");
    }
}

int main() {
    std::string pattern = (fs::temp_directory_path() / "slate-history-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) return 1;
    try {
        exercise(fs::path(pattern) / "navigation.sqlite3");
        fs::remove_all(pattern);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        fs::remove_all(pattern);
        return 1;
    }
}
