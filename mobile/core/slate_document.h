#ifndef SLATE_DOCUMENT_H
#define SLATE_DOCUMENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One owner/thread per handle. Snapshots share immutable pieces and may run
   on another thread after creation. All sizes and offsets refer to bytes. */
typedef struct SlateDocument SlateDocument;
SlateDocument *slate_document_new(void);
SlateDocument *slate_document_snapshot(const SlateDocument *document);
void slate_document_free(SlateDocument *document);
int slate_document_open(SlateDocument *document, const char *path);
int slate_document_open_fd(SlateDocument *document, int fd, const char *display_name);
int slate_document_save(SlateDocument *document, const char *path);
uint64_t slate_document_size(const SlateDocument *document);
int slate_document_dirty(const SlateDocument *document);
int64_t slate_document_read(const SlateDocument *document, uint64_t offset, uint8_t *destination, size_t capacity);
int slate_document_replace(SlateDocument *document, uint64_t offset, uint64_t erased,
                           const uint8_t *inserted, size_t length);
/* First UTF-8 character boundary at or after nominal (clamped to EOF), keeping
   CRLF together. Returns UINT64_MAX on a read error. */
uint64_t slate_document_boundary(const SlateDocument *document, uint64_t nominal);
/* Mutators return 1 on success and 0 on failure. Read returns -1 on failure.
   The returned error message belongs to the handle. */
const char *slate_document_error(const SlateDocument *document);

#ifdef __cplusplus
}
#endif
#endif
