#include "WorkspaceSearch.h"
#include "DirectoryIndex.h"
#include "search.hpp"

#include <memory>
#include <string>
#include <vector>

struct SlateSearch { slate::SearchCancellation cancellation; };

namespace {
struct Entry {
    std::string name;
    bool folder;
};
struct Folder {
    std::string path, prefix;
    std::unique_ptr<SlateDirectory, decltype(&slate_directory_close)> listing;
    std::vector<Entry> matches;
    int64_t offset = 0;
    size_t next = 0;
    bool finished = false;
    Folder(std::string path, std::string prefix, const char* database)
        : path(std::move(path)), prefix(std::move(prefix)),
          listing(slate_directory_open(this->path.c_str(), database), slate_directory_close) {}
};

int64_t searchNames(SlateSearch* scope, const char* root, const char* database, const char* query,
                    size_t limit, void* context, SlateSearchHit hit) {
    std::vector<Folder> stack;
    stack.emplace_back(root, "", database);
    int64_t errors = 0;
    size_t reported = 0;
    while (!stack.empty() && reported < limit && !scope->cancellation.stop_requested()) {
        auto& folder = stack.back();
        if (folder.next == folder.matches.size()) {
            if (folder.finished) { stack.pop_back(); continue; }
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
            stack.emplace_back(std::move(path), std::move(prefix), database);
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
