#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SlateSearch SlateSearch;
typedef void (*SlateSearchHit)(void *context, const char *path, uint64_t offset,
                               size_t length, const char *excerpt);
SlateSearch *slate_search_new(void);
void slate_search_free(SlateSearch *search);
void slate_search_cancel(SlateSearch *search);
// Returns the number of unreadable entries, or -1 if the scan failed.
int64_t slate_search_run(SlateSearch *search, const char *root, const char *query,
                         int contents, void *context, SlateSearchHit hit);

#ifdef __cplusplus
}
#endif
