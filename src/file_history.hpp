#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

struct sqlite3;

namespace slate {

struct HistoryFile {
    std::filesystem::path path;
    std::uint64_t cursor;
    std::optional<bool> preview;
    std::int64_t visit;
};

// An empty database path creates a private, disk-backed temporary history.
class FileHistory {
public:
    explicit FileHistory(const std::filesystem::path& database);
    ~FileHistory();
    FileHistory(const FileHistory&) = delete;
    FileHistory& operator=(const FileHistory&) = delete;

    std::optional<HistoryFile> current() const;
    std::optional<HistoryFile> previous(std::int64_t from = 0) const;
    std::optional<HistoryFile> next(std::int64_t from = 0) const;
    std::optional<HistoryFile> position(const std::filesystem::path& path) const;
    std::vector<HistoryFile> nearby_tabs() const;
    void remember(const std::filesystem::path& path, std::uint64_t cursor, bool preview);
    void visit(const std::filesystem::path& path, std::int64_t existing_visit = 0,
               bool close_current_tab = false);
    void saved_as(const std::filesystem::path& path);
    void forget(const std::filesystem::path& path);

private:
    sqlite3* db_ = nullptr;
    void initialize(const std::filesystem::path& path);
    void change(const std::filesystem::path& path, std::int64_t existing_visit,
                bool replace_visit, bool close_current_tab);
};

} // namespace slate
