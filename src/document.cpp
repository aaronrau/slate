#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "document.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace slate {
namespace {
namespace fs = std::filesystem;
using Offset = std::uint64_t;
static_assert(sizeof(off_t) >= 8);
constexpr Offset max_size = std::numeric_limits<off_t>::max();
constexpr std::size_t block_size = 64 * 1024;

[[noreturn]] void fail(const char* action) {
    throw std::system_error(errno, std::generic_category(), action);
}

class Descriptor {
public:
    explicit Descriptor(int fd) : fd_(fd) {}
    ~Descriptor() { if (fd_ >= 0) ::close(fd_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    int get() const { return fd_; }

private:
    int fd_;
};

struct Revision {
    decltype(std::declval<struct stat>().st_dev) device;
    decltype(std::declval<struct stat>().st_ino) inode;
    off_t length;
    timespec modified;
    timespec changed;
    decltype(std::declval<struct stat>().st_mode) mode;
};

Revision revision(const struct stat& info) {
#ifdef __APPLE__
    return {info.st_dev, info.st_ino, info.st_size, info.st_mtimespec, info.st_ctimespec, info.st_mode};
#else
    return {info.st_dev, info.st_ino, info.st_size, info.st_mtim, info.st_ctim, info.st_mode};
#endif
}

Revision inspect(int fd) {
    struct stat info {};
    if (::fstat(fd, &info) != 0) fail("Inspect file");
    return revision(info);
}

bool sameContentRevision(const Revision& a, const Revision& b) {
    return a.device == b.device && a.inode == b.inode && a.length == b.length &&
           a.modified.tv_sec == b.modified.tv_sec && a.modified.tv_nsec == b.modified.tv_nsec;
}

bool operator==(const Revision& a, const Revision& b) {
    return sameContentRevision(a, b) &&
           a.changed.tv_sec == b.changed.tv_sec && a.changed.tv_nsec == b.changed.tv_nsec;
}

void readFully(int fd, char* data, std::size_t count, Offset offset) {
    while (count) {
        auto n = ::pread(fd, data, count, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) fail("Read file");
        if (n == 0) throw std::runtime_error("File was shortened while it was open");
        data += n;
        offset += n;
        count -= static_cast<std::size_t>(n);
    }
}

void writeFully(int fd, const char* data, std::size_t count, Offset offset) {
    while (count) {
        auto n = ::pwrite(fd, data, count, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) fail("Write file");
        if (n == 0) throw std::runtime_error("File write made no progress");
        data += n;
        offset += n;
        count -= static_cast<std::size_t>(n);
    }
}

enum class FileOrigin { path, provider };

class File {
    Descriptor fd_;
    FileOrigin origin_;

public:
    Revision original;
    File(Descriptor fd, FileOrigin origin)
        : fd_(std::move(fd)), origin_(origin), original(inspect(fd_.get())) {
        if (!S_ISREG(original.mode)) throw std::runtime_error("Choose a regular file");
    }

    void read(Offset start, char* out, std::size_t count) const {
        // Larger reads bypass the display cache. pread never changes a shared offset.
        if (count >= block_size) {
            readFully(fd_.get(), out, count, start);
            return;
        }
        std::lock_guard lock(cache_mutex_);
        while (count) {
            const auto number = start / block_size;
            auto& entry = cache_[number % cache_.size()];
            if (entry.number != number) {
                const Offset base = number * block_size;
                const auto available = std::min<Offset>(block_size, original.length - base);
                if (!entry.bytes) entry.bytes = std::make_unique<char[]>(block_size);
                entry.number = std::numeric_limits<Offset>::max();
                readFully(fd_.get(), entry.bytes.get(), available, base);
                entry.number = number;
            }
            const auto within = static_cast<std::size_t>(start % block_size);
            const auto take = std::min(count, block_size - within);
            std::memcpy(out, entry.bytes.get() + within, take);
            count -= take;
            start += take;
            out += take;
        }
    }

    void verify() const {
        const auto now = inspect(fd_.get());
        // A document provider can change metadata while cataloging an open file.
        const bool unchanged = origin_ == FileOrigin::provider
            ? sameContentRevision(now, original) : now == original;
        if (!unchanged) {
            throw std::runtime_error("File changed on disk. Reopen it before saving.");
        }
    }

    bool hasHoles() const {
#if defined(SEEK_DATA) && defined(SEEK_HOLE)
        const auto hole = ::lseek(fd_.get(), 0, SEEK_HOLE);
        return hole >= 0 && hole < original.length;
#else
        return false;
#endif
    }

    void copyTo(int output, Offset start, Offset count, Offset dest,
                std::vector<char>& buffer, [[maybe_unused]] bool sparse) const {
        const auto end = start + count;
        auto copyData = [&](Offset limit) {
            while (start < limit) {
                const auto take = static_cast<std::size_t>(std::min<Offset>(limit - start, buffer.size()));
                readFully(fd_.get(), buffer.data(), take, start);
                writeFully(output, buffer.data(), take, dest);
                start += take;
                dest += take;
            }
        };
#if defined(SEEK_DATA) && defined(SEEK_HOLE)
        // Preserve holes when editing large sparse files. Filesystems may decline.
        while (sparse && start < end) {
            const auto data = ::lseek(fd_.get(), static_cast<off_t>(start), SEEK_DATA);
            if (data < 0) {
                if (errno == ENXIO) return;
                break;
            }
            if (static_cast<Offset>(data) >= end) return;
            if (static_cast<Offset>(data) < start) break;
            dest += static_cast<Offset>(data) - start;
            start = static_cast<Offset>(data);
            const auto hole = ::lseek(fd_.get(), data, SEEK_HOLE);
            if (hole <= data) break;
            copyData(std::min(end, static_cast<Offset>(hole)));
        }
#endif
        copyData(end);
    }

    void refreshRevision() { original = inspect(fd_.get()); }

private:
    struct Entry {
        Offset number = std::numeric_limits<Offset>::max();
        std::unique_ptr<char[]> bytes;
    };
    mutable std::mutex cache_mutex_;
    mutable std::array<Entry, 8> cache_;
};

// Leaves own the blocks they reference. Removing a range immediately releases
// its unused blocks once readers of older snapshots have finished.
class EditBlock {
public:
    EditBlock() : used_(0) {}

    std::pair<std::size_t, std::size_t> append(std::string_view text) {
        std::lock_guard lock(mutex_);
        const auto start = used_;
        const auto count = std::min(text.size(), bytes_.size() - used_);
        std::memcpy(bytes_.data() + used_, text.data(), count);
        used_ += count;
        return {start, count};
    }

    // Published ranges are immutable. Other snapshots may append past them.
    const char* data() const { return bytes_.data(); }

private:
    std::mutex mutex_;
    std::size_t used_;
    std::array<char, block_size> bytes_;
};

using Block = std::shared_ptr<EditBlock>;
struct Node;
using Tree = std::shared_ptr<const Node>;

// An AVL rope. Branch lengths support byte lookup, and immutable paths make a
// snapshot constant time. Consecutive typing coalesces into one additions leaf.
struct Node {
    Tree left, right;
    Offset length, start;
    int height;
    Block block; // null for a file piece
    bool leaf() const { return !left; }
};

Offset length(const Tree& tree) { return tree ? tree->length : 0; }
int height(const Tree& tree) { return tree ? tree->height : 0; }

Tree piece(const Block& block, Offset start, Offset count) {
    if (!count) return {};
    return std::make_shared<Node>(Node{{}, {}, count, start, 1, block});
}

Tree branch(const Tree& l, const Tree& r) {
    if (!l) return r;
    if (!r) return l;
    return std::make_shared<Node>(Node{l, r, l->length + r->length, 0,
                                  1 + std::max(l->height, r->height), {}});
}

// Called after joining into one child, so a single or double rotation suffices.
Tree balance(const Tree& l, const Tree& r) {
    if (height(l) > height(r) + 1) {
        const auto& middle = l->right;
        if (height(l->left) >= height(middle)) return branch(l->left, branch(middle, r));
        return branch(branch(l->left, middle->left), branch(middle->right, r));
    }
    if (height(r) > height(l) + 1) {
        const auto& middle = r->left;
        if (height(r->right) >= height(middle)) return branch(branch(l, middle), r->right);
        return branch(branch(l, middle->left), branch(middle->right, r->right));
    }
    return branch(l, r);
}

Tree join(const Tree& l, const Tree& r) {
    if (!l || !r) return l ? l : r;
    if (l->height > r->height + 1) return balance(l->left, join(l->right, r));
    if (r->height > l->height + 1) return balance(join(l, r->left), r->right);
    return branch(l, r);
}

Tree buildBalanced(const std::vector<Tree>& pieces, std::size_t first, std::size_t end) {
    if (end - first == 1) return pieces[first];
    const auto mid = first + (end - first) / 2;
    return branch(buildBalanced(pieces, first, mid), buildBalanced(pieces, mid, end));
}

Tree insertPieces(std::weak_ptr<EditBlock>& tail, std::string_view text) {
    if (text.empty()) return {};
    auto current = tail.lock();
    std::vector<Tree> pieces;
    while (!text.empty()) {
        if (!current) current = std::make_shared<EditBlock>();
        const auto [start, count] = current->append(text);
        if (count) {
            auto leaf = piece(current, start, count);
            text.remove_prefix(count);
            if (text.empty()) {
                tail = current;
                if (pieces.empty()) return leaf;
            }
            pieces.push_back(std::move(leaf));
        }
        current.reset();
    }
    return buildBalanced(pieces, 0, pieces.size());
}

std::pair<Tree, Tree> split(const Tree& tree, Offset pos) {
    if (pos == 0) return {{}, tree};
    if (pos == tree->length) return {tree, {}};
    if (tree->leaf()) {
        return {piece(tree->block, tree->start, pos),
                piece(tree->block, tree->start + pos, tree->length - pos)};
    }
    if (pos < tree->left->length) {
        auto [head, middle] = split(tree->left, pos);
        return {head, join(middle, tree->right)};
    }
    auto [middle, tail] = split(tree->right, pos - tree->left->length);
    return {join(tree->left, middle), tail};
}

Tree concatenate(const Tree& l, const Tree& r) {
    if (!l || !r) return l ? l : r;
    auto last = l.get();
    auto first = r.get();
    while (!last->leaf()) last = last->right.get();
    while (!first->leaf()) first = first->left.get();
    if (last->block != first->block || last->start + last->length != first->start) {
        return join(l, r);
    }
    auto combined = piece(last->block, last->start, last->length + first->length);
    return join(join(split(l, l->length - last->length).first, combined),
                split(r, first->length).second);
}

template<class Visitor>
void visit(const Tree& tree, Offset pos, Offset count, Visitor& visitor) {
    if (!count) return;
    if (tree->leaf()) {
        visitor(tree->block, tree->start + pos, count);
        return;
    }
    const auto divider = tree->left->length;
    if (pos >= divider) {
        visit(tree->right, pos - divider, count, visitor);
        return;
    }
    const auto left_count = std::min(divider - pos, count);
    visit(tree->left, pos, left_count, visitor);
    visit(tree->right, 0, count - left_count, visitor);
}

struct Destination {
    fs::path physical;
    std::optional<Revision> existing;
};

Destination destination(const fs::path& path) {
    const auto absolute = fs::absolute(path);
    struct stat info {};
    if (::stat(absolute.c_str(), &info) == 0) {
        if (!S_ISREG(info.st_mode)) throw std::runtime_error("Choose a regular file");
        return {fs::canonical(absolute), revision(info)};
    }
    if (errno != ENOENT) fail("Inspect destination");
    if (::lstat(absolute.c_str(), &info) == 0) throw std::runtime_error("Destination is a broken link");
    if (errno != ENOENT) fail("Inspect destination");
    return {fs::canonical(absolute.parent_path()) / absolute.filename(), {}};
}

class Temporary {
public:
    explicit Temporary(const fs::path& directory)
        : name_((directory / ".slate-XXXXXX").string()), fd_(::mkstemp(name_.data())) {
        if (fd_.get() < 0) fail("Create temporary save file");
        ::fcntl(fd_.get(), F_SETFD, FD_CLOEXEC);
    }
    ~Temporary() { if (!committed_) ::unlink(name_.c_str()); }
    int get() const { return fd_.get(); }
    void commit(const fs::path& target) {
        if (::rename(name_.c_str(), target.c_str()) != 0) fail("Replace destination");
        committed_ = true;
    }
    Descriptor take() { return std::move(fd_); }

private:
    std::string name_;
    Descriptor fd_;
    bool committed_ = false;
};
} // namespace

struct Document::State {
    Tree root;
    std::shared_ptr<File> file;
    std::weak_ptr<EditBlock> tail;
    fs::path display_path, absolute_path, physical_path;
    bool dirty = false;

    State() = default;
    State(Descriptor fd, FileOrigin origin, fs::path display, fs::path absolute = {}, fs::path physical = {})
        : file(std::make_shared<File>(std::move(fd), origin)), display_path(std::move(display)),
          absolute_path(std::move(absolute)), physical_path(std::move(physical)) {
        root = piece({}, 0, file->original.length);
    }
};

Document::Document() : state_(std::make_unique<State>()) {}
Document::Document(std::unique_ptr<State> state) : state_(std::move(state)) {}
Document::~Document() = default;
Document::Document(Document&&) noexcept = default;
Document& Document::operator=(Document&&) noexcept = default;

void Document::open(const fs::path& path) {
    const auto absolute = fs::absolute(path);
    const auto physical = fs::canonical(absolute);
    Descriptor fd(::open(physical.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (fd.get() < 0) fail("Open file");
    state_ = std::make_unique<State>(std::move(fd), FileOrigin::path, path, absolute, physical);
}

void Document::open_descriptor(int descriptor, const fs::path& display_name) {
    Descriptor copy(::fcntl(descriptor, F_DUPFD_CLOEXEC, 0));
    if (copy.get() < 0) fail("Open file descriptor");
    state_ = std::make_unique<State>(std::move(copy), FileOrigin::provider, display_name);
}

Offset Document::size() const { return length(state_->root); }
fs::path Document::path() const { return state_->display_path; }
bool Document::dirty() const { return state_->dirty; }

Document Document::snapshot() const { return Document(std::make_unique<State>(*state_)); }

std::string Document::read(Offset offset, std::size_t count) const {
    if (offset >= size() || !count) return {};
    count = static_cast<std::size_t>(std::min<Offset>(count, size() - offset));
    std::string result(count, '\0');
    read_into(offset, result.data(), count);
    return result;
}

std::size_t Document::read_into(Offset offset, char* out, std::size_t capacity) const {
    if (offset >= size() || !capacity) return 0;
    const auto count = static_cast<std::size_t>(std::min<Offset>(capacity, size() - offset));
    auto append = [&](const Block& block, Offset start, Offset n) {
        if (block) std::memcpy(out, block->data() + start, n);
        else state_->file->read(start, out, n);
        out += n;
    };
    visit(state_->root, offset, count, append);
    return count;
}

void Document::replace(Offset offset, Offset erase_length, std::string_view inserted) {
    if (offset > size()) throw std::out_of_range("Edit starts past the end of the document");
    erase_length = std::min(erase_length, size() - offset);
    if (!erase_length && inserted.empty()) return;
    if (inserted.size() > max_size - (size() - erase_length)) {
        throw std::length_error("Document exceeds the file offset limit");
    }
    auto [before, remainder] = split(state_->root, offset);
    auto after = split(remainder, erase_length).second;
    auto middle = insertPieces(state_->tail, inserted);
    state_->root = concatenate(concatenate(before, middle), after);
    state_->dirty = true;
}

void Document::save(const fs::path& path) {
    if (path.empty()) throw std::invalid_argument("Choose a file name");
    const auto target = destination(path);
    const auto absolute = fs::absolute(path);
    if (state_->file) {
        state_->file->verify();
        if (absolute == state_->absolute_path || target.physical == state_->physical_path) {
            if (target.existing != std::optional<Revision>(state_->file->original)) {
                throw std::runtime_error("Destination changed on disk. Reopen it before saving.");
            }
            if (!dirty()) {
                state_->display_path = path;
                state_->absolute_path = absolute;
                return;
            }
        }
    }

    Temporary temp(target.physical.parent_path());
    std::vector<char> buffer(256 * 1024);
    Offset dest = 0;
    const bool sparse = state_->file && state_->file->hasHoles();
    auto emit = [&](const Block& block, Offset start, Offset count) {
        if (block) writeFully(temp.get(), block->data() + start, count, dest);
        else state_->file->copyTo(temp.get(), start, count, dest, buffer, sparse);
        dest += count;
    };
    visit(state_->root, 0, size(), emit);
    if (::ftruncate(temp.get(), static_cast<off_t>(dest)) != 0) fail("Set saved file length");
    if (target.existing && ::fchmod(temp.get(), target.existing->mode & 0777) != 0) {
        fail("Preserve file permissions");
    }
    if (::fsync(temp.get()) != 0) fail("Flush saved file");

    // Prepare the new model before publishing. The descriptor remains usable even
    // for a destination whose preserved permissions would prevent a second open.
    auto next = std::make_unique<State>();
    next->file = std::make_shared<File>(temp.take(), FileOrigin::path);
    next->root = piece({}, 0, dest);
    next->display_path = path;
    next->absolute_path = absolute;
    next->physical_path = target.physical;
    // Serialize the final check and rename across save workers in this process.
    static std::mutex publish_mutex;
    {
        std::lock_guard lock(publish_mutex);
        const auto now = destination(path);
        if (target.existing != now.existing || target.physical != now.physical) {
            throw std::runtime_error("Destination changed during save");
        }
        if (state_->file) state_->file->verify();
        temp.commit(target.physical);
        next->file->refreshRevision(); // rename can update ctime
    }
    state_ = std::move(next);
}

} // namespace slate
