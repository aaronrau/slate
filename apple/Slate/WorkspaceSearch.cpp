#include "WorkspaceSearch.h"
#include "search.hpp"

struct SlateSearch {
    slate::SearchCancellation cancellation;
};

SlateSearch *slate_search_new(void) {
    try { return new SlateSearch; } catch (...) { return nullptr; }
}
void slate_search_free(SlateSearch *search) { delete search; }
void slate_search_cancel(SlateSearch *search) { search->cancellation.request_stop(); }
int64_t slate_search_run(SlateSearch *search, const char *root, const char *query,
                         int contents, void *context, SlateSearchHit hit) {
    try {
        const auto mode = contents ? slate::SearchMode::contents : slate::SearchMode::filenames;
        const auto summary = slate::search_workspace(root, query, mode, 200, search->cancellation,
            [&](slate::SearchHit result) {
                hit(context, result.path.c_str(), result.offset, result.length, result.excerpt.c_str());
            });
        return static_cast<int64_t>(summary.errors);
    } catch (...) {
        return -1;
    }
}
