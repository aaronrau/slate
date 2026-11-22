#include "slate_document.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
using Handle = std::unique_ptr<SlateDocument, decltype(&slate_document_free)>;
void check(bool result, const char* reason) {
    if (!result) throw std::runtime_error(reason);
}
void exercise(const std::filesystem::path& dir) {
    constexpr uint64_t offset = (uint64_t{5} << 30) + 16;
    auto source = (dir / "document-XXXXXX").string();
    int fd = mkstemp(source.data());
    check(fd >= 0, "could not create test file");
    const std::string tail = "✍📓\r\nEnd";
    check(pwrite(fd, tail.data(), tail.size(), offset) == static_cast<ssize_t>(tail.size()), "write sparse tail");
    Handle doc(slate_document_new(), slate_document_free);
    check(slate_document_open_fd(doc.get(), fd, "note.md"), "open document provider descriptor");
    close(fd);
    check(slate_document_size(doc.get()) == offset + tail.size(), "64-bit file size");
    check(slate_document_boundary(doc.get(), offset + 2) == offset + 3, "three-byte character");
    check(slate_document_boundary(doc.get(), offset + 4) == offset + 7, "four-byte character");
    check(slate_document_boundary(doc.get(), offset + 8) == offset + 9, "CRLF");
    check(slate_document_boundary(doc.get(), UINT64_MAX) == slate_document_size(doc.get()), "EOF boundary");
    auto* raw = slate_document_snapshot(doc.get());
    Handle snapshot(raw, slate_document_free);
    check(snapshot != nullptr, "snapshot");
    std::array<uint8_t, 32> out{};
    const uint8_t replacement[] = {'O','K'};
    check(slate_document_replace(doc.get(), offset + 9, 3, replacement, sizeof(replacement)), "edit above 4 GiB");
    check(slate_document_dirty(doc.get()) && !slate_document_dirty(snapshot.get()), "independent editable snapshots");
    check(slate_document_read(snapshot.get(), offset, out.data(), out.size()) == static_cast<int64_t>(tail.size()),
          "bounded read");
    check(std::string(reinterpret_cast<char*>(out.data()), tail.size()) == tail, "read original snapshot");
    check(!slate_document_open(doc.get(), (dir / "missing").c_str()) && *slate_document_error(doc.get()), "failed open");
    check(slate_document_read(doc.get(), offset + 9, out.data(), out.size()) == 2 && out[0] == 'O', "edit survives failed open");

    // Save a small document so systems without sparse hole reporting need no large disk allocation.
    check(slate_document_replace(doc.get(), 0, offset + 9, nullptr, 0), "delete original prefix");
    auto target = dir / "edited.md";
    check(slate_document_save(doc.get(), target.c_str()) && !slate_document_dirty(doc.get()), "save provider edit");
    check(slate_document_open(doc.get(), target.c_str()) && slate_document_size(doc.get()) == 2, "reopen edited file");
}
}

int main() {
    char directory[] = "/tmp/slate-mobile-XXXXXX";
    if (!mkdtemp(directory)) return 1;
    int result = 0;
    try { exercise(directory); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    std::filesystem::remove_all(directory);
    return result;
}
