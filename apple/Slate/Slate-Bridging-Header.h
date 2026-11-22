#include "slate_document.h"
#include "WorkspaceSearch.h"
#include "DirectoryIndex.h"

#include <fcntl.h>
static inline int slate_openat(int parent, const char *name, int flags, mode_t mode) {
    return openat(parent, name, flags, mode);
}
