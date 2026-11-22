#include "slate_document.h"
#include "../../src/document.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

struct SlateDocument {
    slate::Document core;
    mutable char error[512] = {};
};

namespace {
template<class Action>
bool attempt(const SlateDocument* doc, Action action) noexcept {
    if (!doc) return false;
    try {
        action();
        doc->error[0] = '\0';
        return true;
    } catch (const std::exception& e) {
        std::snprintf(doc->error, sizeof(doc->error), "%s", e.what());
    } catch (...) {
        std::snprintf(doc->error, sizeof(doc->error), "Document operation failed");
    }
    return false;
}
}

SlateDocument* slate_document_new(void) {
    try { return new SlateDocument{}; }
    catch (...) { return nullptr; }
}

SlateDocument* slate_document_snapshot(const SlateDocument* doc) {
    SlateDocument* copy = nullptr;
    attempt(doc, [&] { copy = new SlateDocument{doc->core.snapshot()}; });
    return copy;
}

void slate_document_free(SlateDocument* doc) { delete doc; }
const char* slate_document_error(const SlateDocument* doc) { return doc ? doc->error : "Missing document handle"; }
uint64_t slate_document_size(const SlateDocument* doc) { return doc ? doc->core.size() : 0; }
int slate_document_dirty(const SlateDocument* doc) { return doc && doc->core.dirty(); }

int slate_document_open(SlateDocument* doc, const char* path) {
    return attempt(doc, [&] {
        if (!path || !*path) throw std::invalid_argument("Choose a file");
        doc->core.open(path);
    });
}

int slate_document_open_fd(SlateDocument* doc, int fd, const char* name) {
    return attempt(doc, [&] { doc->core.open_descriptor(fd, name ? name : ""); });
}

int slate_document_save(SlateDocument* doc, const char* path) {
    return attempt(doc, [&] { doc->core.save(path ? path : ""); });
}

int64_t slate_document_read(const SlateDocument* doc, uint64_t offset, uint8_t* dst, size_t capacity) {
    int64_t result = -1;
    attempt(doc, [&] {
        if (capacity && !dst) throw std::invalid_argument("Missing read buffer");
        result = static_cast<int64_t>(doc->core.read_into(offset, reinterpret_cast<char*>(dst), capacity));
    });
    return result;
}

int slate_document_replace(SlateDocument* doc, uint64_t offset, uint64_t erased,
                           const uint8_t* inserted, size_t length) {
    return attempt(doc, [&] {
        if (length && !inserted) throw std::invalid_argument("Missing inserted text");
        doc->core.replace(offset, erased, {inserted ? reinterpret_cast<const char*>(inserted) : "", length});
    });
}

uint64_t slate_document_boundary(const SlateDocument* doc, uint64_t nominal) {
    uint64_t result = UINT64_MAX;
    attempt(doc, [&] {
        const auto end = doc->core.size();
        if (nominal == 0 || nominal >= end) {
            result = std::min(nominal, end);
            return;
        }
        const auto sample = doc->core.read(nominal - 1, 5);
        size_t i = 1;
        if (sample[0] == '\r' && sample[1] == '\n') ++i;
        else while (i < sample.size() && i <= 3 && (static_cast<uint8_t>(sample[i]) & 0xc0) == 0x80) ++i;
        result = nominal + i - 1;
    });
    return result;
}
