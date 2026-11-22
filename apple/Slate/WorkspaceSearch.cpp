#include "WorkspaceSearch.h"
#include "DirectoryIndex.h"
#include "search.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>

struct SlateSearch { slate::SearchCancellation cancellation; };

namespace {
struct Entry {
    std::string name;
    bool folder;
};
// Most source trees contain many small folders. Use one indexed lookup per
// folder, then scan small, uncached folders without an SQLite connection each.
class SnapshotLookup {
    sqlite3* db = nullptr;
    sqlite3_stmt* lookup = nullptr;
public:
    explicit SnapshotLookup(const char* database) {
        if (database && *database && sqlite3_open_v2(database, &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK)
            sqlite3_prepare_v2(db, "SELECT 1 FROM folders WHERE path=?", -1, &lookup, nullptr);
    }
    ~SnapshotLookup() { sqlite3_finalize(lookup); sqlite3_close(db); }
    bool contains(const std::string& path) {
        if (!lookup) return false;
        sqlite3_bind_text(lookup, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        const bool found = sqlite3_step(lookup) == SQLITE_ROW;
        sqlite3_reset(lookup);
        return found;
    }
};
std::string lowerASCII(std::string value) {
    for (auto& ch : value) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return value;
}
struct Folder {
    std::string path, prefix;
    std::unique_ptr<SlateDirectory, decltype(&slate_directory_close)> listing{nullptr, slate_directory_close};
    std::vector<Entry> matches;
    int64_t offset = 0;
    size_t next = 0;
    bool finished = false;
    bool error = false;

    Folder(std::string path, std::string prefix, const char* database, SnapshotLookup& known, const std::string& needle)
        : path(std::move(path)), prefix(std::move(prefix)) {
        if (!known.contains(this->path) && readSmall(needle)) return;
        listing.reset(slate_directory_open(this->path.c_str(), database));
    }

    bool readSmall(const std::string& needle) {
        const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) { finished = error = true; return true; }
        std::unique_ptr<DIR, decltype(&closedir)> stream(fdopendir(fd), closedir);
        if (!stream) { close(fd); finished = error = true; return true; }
        int rows = 0;
        while (rows <= 128) {
            errno = 0;
            const auto* entry = readdir(stream.get());
            if (!entry) {
                if (errno == EINTR) continue;
                finished = true;
                error = errno != 0;
                return true;
            }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            ++rows;
            auto type = entry->d_type;
            if (type == DT_UNKNOWN) {
                struct stat info{};
                if (fstatat(fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW)) continue;
                type = S_ISREG(info.st_mode) ? DT_REG : S_ISDIR(info.st_mode) ? DT_DIR : DT_UNKNOWN;
            }
            if (type == DT_DIR || (type == DT_REG && lowerASCII(prefix + entry->d_name).find(needle) != std::string::npos))
                matches.push_back({entry->d_name, type == DT_DIR});
        }
        matches.clear();
        return false;
    }
};

int64_t searchNames(SlateSearch* scope, const char* root, const char* database, const char* query,
                    size_t limit, void* context, SlateSearchHit hit) {
    std::vector<Folder> stack;
    SnapshotLookup known(database);
    auto folded = lowerASCII(query);
    stack.emplace_back(root, "", database, known, folded);
    int64_t errors = 0;
    size_t reported = 0;
    while (!stack.empty() && reported < limit && !scope->cancellation.stop_requested()) {
        auto& folder = stack.back();
        if (folder.next == folder.matches.size()) {
            if (folder.finished) { errors += folder.error; stack.pop_back(); continue; }
            folder.next = 0;
            folder.matches.clear();
            constexpr int batch = 256;
            const auto n = slate_directory_matches(folder.listing.get(), folder.offset, batch,
                folder.prefix.c_str(), query, &folder.matches, [](void* rows, const char* name, int directory) {
                    static_cast<std::vector<Entry>*>(rows)->push_back({name, directory != 0});
                });
            if (n < 0) { ++errors; stack.pop_back(); continue; }
            folder.offset += n;
            folder.finished = n < batch;
            continue;
        }
        const auto& entry = folder.matches[folder.next++];
        auto path = folder.path + "/" + entry.name;
        if (entry.folder) {
            auto prefix = folder.prefix + entry.name + "/";
            stack.emplace_back(std::move(path), std::move(prefix), database, known, folded);
        } else {
            hit(context, path.c_str(), 0, 0, "");
            ++reported;
        }
    }
    return errors;
}
}

SlateSearch *slate_search_new(void) {
    try { return new SlateSearch; } catch (...) { return nullptr; }
}
void slate_search_free(SlateSearch *search) { delete search; }
void slate_search_cancel(SlateSearch *search) { search->cancellation.request_stop(); }
int64_t slate_search_run(SlateSearch *search, const char *root, const char *database, const char *query,
                         int contents, size_t limit, void *context, SlateSearchHit hit) {
    try {
        if (!contents) return searchNames(search, root, database, query, limit, context, hit);
        const auto summary = slate::search_workspace(root, query, slate::SearchMode::text_contents, limit, search->cancellation,
            [&](slate::SearchHit result) {
                hit(context, result.path.c_str(), result.offset, result.length, result.excerpt.c_str());
            });
        return static_cast<int64_t>(summary.errors);
    } catch (...) {
        return -1;
    }
}
