#include "search.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace slate {
namespace {

namespace fs = std::filesystem;
constexpr std::size_t block_size = 256 * 1024;
static_assert(sizeof(off_t) >= 8, "Slate requires 64-bit file offsets");

char fold_ascii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

struct File {
    int fd;
    ~File() { if (fd >= 0) ::close(fd); }
};

std::string excerpt(std::string_view buffer, std::size_t start, std::size_t length) {
    auto begin = start;
    while (begin > 0 && start - begin < 36 && buffer[begin - 1] != '\n' &&
           buffer[begin - 1] != '\r') --begin;
    auto end = start;
    const auto max_length = std::min<std::size_t>(length, 60) + 52;
    while (end < buffer.size() && end - start < max_length && buffer[end] != '\n' &&
           buffer[end] != '\r') ++end;
    auto text = std::string(buffer.substr(begin, end - begin));
    for (auto& c : text) if (static_cast<unsigned char>(c) < ' ') c = ' ';
    return text;
}

// A hole consists of zero bytes. A query containing no zero cannot cross it.
// SEEK_DATA is a hint; filesystems without it use the sequential reader.
off_t next_data(int fd, off_t from, off_t size, bool& use_extents) {
#if defined(SEEK_DATA) && defined(SEEK_HOLE)
    if (use_extents) {
        auto next = ::lseek(fd, from, SEEK_DATA);
        if (next >= from) return std::min(next, size);
        if (next < 0 && errno == ENXIO) return size;
        use_extents = false;
    }
#else
    (void)fd; (void)size; (void)use_extents;
#endif
    return from;
}

off_t extent_end(int fd, off_t from, off_t size, bool& use_extents) {
#if defined(SEEK_DATA) && defined(SEEK_HOLE)
    if (use_extents) {
        auto hole = ::lseek(fd, from, SEEK_HOLE);
        if (hole > from) return std::min(hole, size);
        use_extents = false;
    }
#else
    (void)fd; (void)from; (void)use_extents;
#endif
    return size;
}

void scan_file(int directory, const char* name, const fs::path& root, const std::string& relative,
               std::string_view query, std::size_t limit,
               const SearchCancellation& stop, const HitCallback& on_hit, SearchSummary& summary,
               std::vector<char>& buffer) {
    File file{::openat(directory, name, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW)};
    if (file.fd < 0) { ++summary.errors; return; }
    struct stat st{};
    if (::fstat(file.fd, &st) != 0) { ++summary.errors; return; }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
        static_cast<std::uint64_t>(st.st_size) < query.size()) return;

    // Allocated files can be sparse too. Query extents only when a file appears
    // sparse, to avoid two extra syscalls for every ordinary workspace file.
    bool use_extents = static_cast<std::uint64_t>(st.st_blocks) * 512 <
                       static_cast<std::uint64_t>(st.st_size) &&
                       query.find('\0') == std::string_view::npos;
    const auto chunk = std::max(block_size, query.size());
    const auto capacity = chunk + query.size() - 1;
    if (buffer.size() < capacity) buffer.resize(capacity);

    off_t pos = 0;
    while (pos < st.st_size && !stop.stop_requested()) {
        pos = next_data(file.fd, pos, st.st_size, use_extents);
        if (pos == st.st_size) break;
        const off_t end = extent_end(file.fd, pos, st.st_size, use_extents);
        std::size_t carry = 0;
        auto next_match = static_cast<std::uint64_t>(pos);
        while (pos < end && !stop.stop_requested()) {
            const auto wanted = std::min<std::uint64_t>(chunk, static_cast<std::uint64_t>(end - pos));
            auto n = ::pread(file.fd, buffer.data() + carry, wanted, pos);
            if (n == 0) return;
            if (n < 0) {
                if (errno == EINTR) continue;
                ++summary.errors;
                return;
            }
            const auto size = carry + static_cast<std::size_t>(n);
            const auto base = static_cast<std::uint64_t>(pos) - carry;
            auto cursor = static_cast<std::size_t>(std::max(base, next_match) - base);
            while (cursor < size && !stop.stop_requested()) {
                const auto* found = static_cast<const char*>(
                    ::memmem(buffer.data() + cursor, size - cursor, query.data(), query.size()));
                if (!found) break;
                const auto index = static_cast<std::size_t>(found - buffer.data());
                on_hit({root / relative, base + index, query.size(),
                        excerpt({buffer.data(), size}, index, query.size())});
                if (++summary.matches >= limit) { summary.reached_limit = true; return; }
                cursor = index + query.size();
                next_match = base + cursor;
            }
            pos += n;
            carry = std::min(size, query.size() - 1);
            if (carry > 0) std::memmove(buffer.data(), buffer.data() + size - carry, carry);
        }
    }
}

struct CloseDirectory {
    void operator()(DIR* stream) const { ::closedir(stream); }
};
using Directory = std::unique_ptr<DIR, CloseDirectory>;
struct Level {
    Directory directory;
    std::size_t prefix_length;
};

// Both target platforms return the entry type with readdir on local filesystems.
// Ask for metadata only when the filesystem omitted the type.
unsigned char entry_type(int parent, const dirent& entry, SearchSummary& summary) {
    if (entry.d_type != DT_UNKNOWN) return entry.d_type;
    struct stat info{};
    if (::fstatat(parent, entry.d_name, &info, AT_SYMLINK_NOFOLLOW) < 0) {
        ++summary.errors;
        return DT_UNKNOWN;
    }
    if (S_ISDIR(info.st_mode)) return DT_DIR;
    return S_ISREG(info.st_mode) ? DT_REG : DT_UNKNOWN;
}

} // namespace

SearchSummary search_workspace(const fs::path& root, std::string_view query, SearchMode mode,
                               std::size_t limit, const SearchCancellation& stop,
                               const HitCallback& on_hit) {
    SearchSummary summary;
    if (query.empty() || limit == 0 || stop.stop_requested()) return summary;
    std::vector<char> buffer;
    std::string folded;
    if (mode == SearchMode::filenames) {
        folded = query;
        std::transform(folded.begin(), folded.end(), folded.begin(), fold_ascii);
    }
    Directory first(::opendir(root.c_str()));
    if (!first) { ++summary.errors; return summary; }
    std::vector<Level> dirs;
    dirs.push_back({std::move(first), 0});
    std::string relative, folded_path;
    while (!dirs.empty() && !stop.stop_requested() && !summary.reached_limit) {
        const auto& parent = dirs.back();
        errno = 0;
        auto* entry = ::readdir(parent.directory.get());
        if (!entry) {
            if (errno == EINTR) continue;
            if (errno) ++summary.errors;
            dirs.pop_back();
            continue;
        }
        const std::string_view name(entry->d_name);
        if (name == "." || name == "..") continue;
        const int fd = ::dirfd(parent.directory.get());
        const auto type = entry_type(fd, *entry, summary);
        if (type != DT_DIR && type != DT_REG) continue;
        relative.resize(parent.prefix_length);
        if (!relative.empty()) relative += '/';
        relative += name;
        if (type == DT_DIR) {
            int child = ::openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) { ++summary.errors; continue; }
            Directory stream(::fdopendir(child));
            if (!stream) { ::close(child); ++summary.errors; continue; }
            dirs.push_back({std::move(stream), relative.size()});
        } else if (mode == SearchMode::contents) {
            scan_file(fd, entry->d_name, root, relative, query, limit, stop, on_hit, summary, buffer);
        } else {
            folded_path.resize(relative.size());
            std::transform(relative.begin(), relative.end(), folded_path.begin(), fold_ascii);
            if (folded_path.find(folded) == std::string::npos) continue;
            on_hit({root / relative, 0, 0, {}});
            summary.reached_limit = ++summary.matches >= limit;
        }
    }
    return summary;
}

} // namespace slate
