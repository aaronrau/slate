#include "search.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <iterator>
#include <memory>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

// Apple's memmem scans dense text at about 1.4 GB/s on an M2, a tenth of the rate
// pread delivers from the page cache, so content search there uses a vector pair scan.
#if defined(__APPLE__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define SLATE_PAIR_SEARCH 1
#elif defined(__APPLE__) && defined(__SSE2__)
#include <emmintrin.h>
#define SLATE_PAIR_SEARCH 1
#endif

namespace slate {
namespace {

namespace fs = std::filesystem;
constexpr std::size_t block_size = 256 * 1024;
static_assert(sizeof(off_t) >= 8, "Slate requires 64-bit file offsets");

#ifdef SLATE_PAIR_SEARCH
// One bit per byte position (NEON: one nibble) where the needle's first byte and
// its last byte, needle.size() - 1 further on, both match.
#ifdef __ARM_NEON
constexpr int lane_shift = 2;
constexpr std::uint64_t lane_bits = 0xF;
inline std::uint64_t pair_mask(const char* at, std::size_t last, uint8x16_t first_byte, uint8x16_t last_byte) {
    const auto head = vceqq_u8(vld1q_u8(reinterpret_cast<const std::uint8_t*>(at)), first_byte);
    const auto tail = vceqq_u8(vld1q_u8(reinterpret_cast<const std::uint8_t*>(at + last)), last_byte);
    const auto both = vandq_u8(head, tail);
    return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(both), 4)), 0);
}
#else
constexpr int lane_shift = 0;
constexpr std::uint64_t lane_bits = 1;
inline std::uint64_t pair_mask(const char* at, std::size_t last, __m128i first_byte, __m128i last_byte) {
    const auto head = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(at)), first_byte);
    const auto tail = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(at + last)), last_byte);
    return static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_and_si128(head, tail)));
}
#endif

// The first occurrence of needle in haystack, as memmem. A pair that matches but
// isn't the needle costs a memcmp; when those outnumber one per 16 bytes scanned
// (a repetitive needle and file), the rest goes to memmem, which stays linear.
const char* find_bytes(const char* haystack, std::size_t size, std::string_view needle) {
    const auto length = needle.size();
    if (length < 2 || size < length) return static_cast<const char*>(::memmem(haystack, size, needle.data(), length));
    const auto last = length - 1;
#ifdef __ARM_NEON
    const auto first_byte = vdupq_n_u8(static_cast<std::uint8_t>(needle.front()));
    const auto last_byte = vdupq_n_u8(static_cast<std::uint8_t>(needle.back()));
#else
    const auto first_byte = _mm_set1_epi8(needle.front());
    const auto last_byte = _mm_set1_epi8(needle.back());
#endif
    std::size_t at = 0, misses = 0;
    // Every candidate start in [at, at + 16) leaves room for the whole needle.
    while (at + last + 16 <= size) {
        for (auto mask = pair_mask(haystack + at, last, first_byte, last_byte); mask; ) {
            const auto lane = static_cast<std::size_t>(__builtin_ctzll(mask)) >> lane_shift;
            const auto* candidate = haystack + at + lane;
            if (std::memcmp(candidate + 1, needle.data() + 1, length - 2) == 0) return candidate;
            if (++misses > 64 + at / 16) {
                return static_cast<const char*>(::memmem(haystack + at, size - at, needle.data(), length));
            }
            mask &= ~(lane_bits << (lane << lane_shift));
        }
        at += 16;
    }
    return static_cast<const char*>(::memmem(haystack + at, size - at, needle.data(), length));
}
#else
const char* find_bytes(const char* haystack, std::size_t size, std::string_view needle) {
    return static_cast<const char*>(::memmem(haystack, size, needle.data(), needle.size()));
}
#endif

char fold_ascii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

bool binary_filename(std::string_view name) {
    // Some providers put the duplicate number after the extension, e.g. "clip.wav (1)".
    if (!name.empty() && name.back() == ')') {
        const auto suffix = name.rfind(" (");
        if (suffix != name.npos) {
            const auto digits = name.substr(suffix + 2, name.size() - suffix - 3);
            if (!digits.empty() && digits.find_first_not_of("0123456789") == digits.npos)
                name.remove_suffix(name.size() - suffix);
        }
    }
    const auto dot = name.rfind('.');
    if (dot == name.npos) return false;
    const auto extension = name.substr(dot + 1);
    static constexpr std::string_view binary[] = {
        "wav", "mp3", "m4a", "aac", "flac", "opus", "ogg", "aif", "aiff", "amr", "au",
        "caf", "mka", "oga", "pcm", "wma", "3gp", "avi", "m4v", "mkv", "mov", "mp4",
        "mpeg", "webm", "bmp", "gif", "heic", "jpeg", "jpg", "png", "tiff", "webp",
        "7z", "bz2", "gz", "pdf", "rar", "sqlite", "sqlite3", "xz", "zip"
    };
    return std::any_of(std::begin(binary), std::end(binary), [&](std::string_view format) {
        return extension.size() == format.size() &&
               std::equal(extension.begin(), extension.end(), format.begin(),
                          [](char c, char lower) { return fold_ascii(c) == lower; });
    });
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
               std::string_view query, bool text_only, std::size_t limit,
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
    bool sample = text_only;
    while (pos < st.st_size && !stop.stop_requested()) {
        pos = next_data(file.fd, pos, st.st_size, use_extents);
        if (pos == st.st_size) break;
        const off_t end = extent_end(file.fd, pos, st.st_size, use_extents);
        std::size_t carry = 0;
        auto next_match = static_cast<std::uint64_t>(pos);
        while (pos < end && !stop.stop_requested()) {
            // Reuse the text probe as the first searchable block, including matches
            // that cross its end. Unknown binary files need at most one small read.
            const auto read_size = sample ? std::min<std::size_t>(8192, chunk) : chunk;
            const auto wanted = std::min<std::uint64_t>(read_size, static_cast<std::uint64_t>(end - pos));
            auto n = ::pread(file.fd, buffer.data() + carry, wanted, pos);
            if (n == 0) return;
            if (n < 0) {
                if (errno == EINTR) continue;
                ++summary.errors;
                return;
            }
            if (sample && std::memchr(buffer.data() + carry, 0, n)) return;
            sample = false;
            const auto size = carry + static_cast<std::size_t>(n);
            const auto base = static_cast<std::uint64_t>(pos) - carry;
            auto cursor = static_cast<std::size_t>(std::max(base, next_match) - base);
            while (cursor < size && !stop.stop_requested()) {
                const auto* found = find_bytes(buffer.data() + cursor, size - cursor, query);
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
        } else if (mode != SearchMode::filenames) {
            const bool text_only = mode == SearchMode::text_contents;
            if (!text_only || !binary_filename(name)) {
                scan_file(fd, entry->d_name, root, relative, query, text_only, limit,
                          stop, on_hit, summary, buffer);
            }
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
