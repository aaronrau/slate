#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SlateDirectory SlateDirectory;
typedef void (*SlateDirectoryRow)(void *, const char *name, int directory);
SlateDirectory *slate_directory_open(const char *path, const char *database);
void slate_directory_close(SlateDirectory *directory);
// A negative result indicates an error. count is unknown (-1) until the end.
int slate_directory_page(SlateDirectory *, int64_t offset, int limit, void *, SlateDirectoryRow);
// Emits child folders and matching relative file names from one metadata page.
int slate_directory_matches(SlateDirectory *, int64_t offset, int limit, const char *prefix,
                            const char *needle, void *, SlateDirectoryRow);
int slate_directory_warm(SlateDirectory *, int batch_size);
int64_t slate_directory_count(const SlateDirectory *);
int64_t slate_directory_scanned(const SlateDirectory *);
int slate_directory_cached(const SlateDirectory *);
const char *slate_directory_error(const SlateDirectory *);
void slate_directory_forget(const char *database, const char *root);

#ifdef __cplusplus
}
#endif
