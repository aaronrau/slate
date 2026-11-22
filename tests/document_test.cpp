#include "document.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using slate::Document;
using namespace std::string_literals;

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class F> void expectError(F action) {
    try {
        action();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("Expected operation to fail");
}

struct Scratch {
    fs::path path;
    Scratch() {
        std::string pattern = (fs::temp_directory_path() / "slate-test-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
        path = pattern;
    }
    ~Scratch() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void writeFile(const fs::path& path, const std::string& data) {
    std::ofstream file(path, std::ios::binary);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    file.close();
}

std::string contents(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

void basicEdits(const fs::path& dir) {
    const auto path = dir / "basic";
    const auto link = dir / "link";
    Document doc;
    check(!doc.dirty() && doc.size() == 0 && doc.path().empty(), "empty initial state");
    doc.replace(0, 5, "");
    check(!doc.dirty(), "empty replacement should be clean");
    check(doc.read(100, 10).empty(), "reads beyond EOF");
    expectError([&] { doc.replace(1, 0, "a"); });
    const auto bytes = "a\0\r\nz"s;
    doc.replace(0, 0, bytes);
    check(doc.size() == bytes.size() && doc.dirty(), "inserted byte count");
    doc.save(path);
    check(doc.path() == path && !doc.dirty() && contents(path) == bytes, "create and save");
    check(::chmod(path.c_str(), 0740) == 0, "chmod fixture");
    fs::create_symlink(path.filename(), link);
    doc.open(link);
    doc.replace(1, 1, "xyz");
    doc.replace(doc.size() - 1, std::numeric_limits<std::uint64_t>::max(), "Z");
    doc.save(link);
    check(fs::is_symlink(link), "save preserves symlink");
    check(contents(path) == "axyz\r\nZ", "save reaches symlink target");
    struct stat info {};
    check(::stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0740, "preserved mode");
    doc.replace(0, doc.size(), "");
    doc.save(path);
    check(contents(path).empty() && !doc.dirty(), "empty save");
    doc.replace(0, 0, "retain after failed open");
    expectError([&] { doc.open(dir / "absent"); });
    expectError([&] { doc.open(dir); });
    check(doc.read(0, 100) == "retain after failed open", "failed open leaves model usable");
    expectError([&] { doc.save(dir); });
    check(doc.dirty(), "failed save preserves dirty state");
}

void randomEdits(const fs::path& dir) {
    std::mt19937 rng(4021);
    const auto path = dir / "random";
    std::string expected(150000, ' ');
    for (char& c : expected) c = static_cast<char>(rng());
    writeFile(path, expected);
    Document doc;
    doc.open(path);
    for (int i = 0; i < 18000; ++i) {
        const auto pos = rng() % (expected.size() + 1);
        const auto erase = rng() % 20;
        std::string insert(rng() % 24, ' ');
        for (char& c : insert) c = static_cast<char>(rng());
        doc.replace(pos, erase, insert);
        expected.replace(pos, erase, insert);
        check(doc.size() == expected.size(), "differential length");
        const auto start = rng() % (expected.size() + 1);
        const auto count = rng() % 190;
        check(doc.read(start, count) == expected.substr(start, count), "differential seek");
        if (i % 1000 == 0) check(doc.read(0, doc.size()) == expected, "differential full read");
    }
    doc.save(path);
    check(contents(path) == expected, "save fragmented document");
    check(doc.read(0, expected.size()) == expected, "read rebound document");
}

void largePaste(const fs::path& dir) {
    std::string text(3 * 65536 + 17, ' ');
    for (std::size_t i = 0; i < text.size(); ++i) text[i] = static_cast<char>(i % 251);
    Document doc;
    doc.replace(0, 0, "head");
    doc.replace(4, 0, text);
    auto snapshot = doc.snapshot();
    doc.replace(5, text.size() - 2, "");
    check(doc.read(0, doc.size()) == "head" + text.substr(0, 1) + text.back(),
          "keep ends of a multi-block paste");
    check(snapshot.read(4, text.size()) == text, "snapshot owns removed blocks");
    snapshot.save(dir / "paste");
    check(contents(dir / "paste") == "head" + text, "save multi-block paste");
}

void snapshots(const fs::path& dir) {
    auto path = dir / "snapshot";
    const auto output = dir / "worker";
    std::string original(3 * 65536, 'q');
    writeFile(path, original);
    Document doc;
    doc.open(path);
    doc.replace(65535, 2, "12");
    original.replace(65535, 2, "12");
    auto worker = doc.snapshot();
    std::atomic<bool> started = false;
    std::exception_ptr error;
    std::thread thread([&] {
        try {
            started = true;
            for (int i = 0; i < 2000; ++i) {
                worker.replace(worker.size(), 0, "y");
                if (worker.read(65529, 22) != original.substr(65529, 22)) {
                    throw std::runtime_error("snapshot cross-page read");
                }
            }
            worker.save(output);
        } catch (...) { error = std::current_exception(); }
    });
    while (!started) std::this_thread::yield();
    for (int i = 0; i < 2000; ++i) {
        doc.replace(doc.size(), 0, "x");
        doc.read(65528, 30);
    }
    thread.join();
    if (error) std::rethrow_exception(error);
    check(contents(output) == original + std::string(2000, 'y'), "worker snapshot");
    check(doc.read(0, doc.size()) == original + std::string(2000, 'x'), "independent edit store");
    doc = std::move(worker);
    check(!doc.dirty() && doc.path() == output, "install saved worker model");
}

void conflicts(const fs::path& dir) {
    const auto path = dir / "changed";
    writeFile(path, std::string(5 * 65536, 'x'));
    Document doc;
    doc.open(path);
    doc.replace(0, 0, "local");
    writeFile(path, "external");
    expectError([&] { doc.read(4 * 65536, 3); });
    expectError([&] { doc.save(path); });
    check(contents(path) == "external" && doc.dirty(), "reject truncated source");
    doc.open(path);
    doc.replace(0, 0, "local");
    fs::rename(path, dir / "old-inode");
    writeFile(path, "replacement");
    expectError([&] { doc.save(path); });
    check(contents(path) == "replacement", "reject replaced destination");
    doc.open(path);
    doc.replace(0, 1, "R");
    fs::remove(path);
    expectError([&] { doc.save(path); });
    check(!fs::exists(path), "reject removed destination");
}

void providerRevisions(const fs::path& dir) {
    const auto source = dir / "provider-source";
    const auto output = dir / "provider-copy";
    writeFile(source, "abcd");
    timespec times[2] = {{0, UTIME_OMIT}, {1700000000, 100000000}};
    check(::utimensat(AT_FDCWD, source.c_str(), times, 0) == 0, "set initial modification time");

    Document named, provider;
    named.open(source);
    const int fd = ::open(source.c_str(), O_RDONLY);
    check(fd >= 0, "open provider fixture");
    provider.open_descriptor(fd, "document.txt");
    ::close(fd);
    provider.replace(1, 1, "B");
    auto worker = provider.snapshot();

    // Give filesystems with a coarse ctime clock a distinct tick for chmod.
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    check(::chmod(source.c_str(), 0700) == 0, "change provider metadata");
    expectError([&] { named.save(source); });
    worker.save(output);
    check(contents(output) == "aBcd", "provider metadata change blocked the edited snapshot");

    writeFile(source, "wxyz");
    times[1].tv_nsec += 100000000; // Same length and mtime seconds, different subsecond revision.
    check(::utimensat(AT_FDCWD, source.c_str(), times, 0) == 0, "set new modification time");
    expectError([&] { provider.save(output); });
    check(provider.dirty() && contents(output) == "aBcd" && contents(source) == "wxyz",
          "provider conflict must retain unsaved edits and both files");
}

void concurrentSave(const fs::path& dir) {
    const auto path = dir / "two-writers";
    writeFile(path, "source");
    Document a;
    a.open(path);
    auto b = a.snapshot();
    a.replace(0, a.size(), "first revision");
    b.replace(0, b.size(), "second revision");
    std::atomic<int> ready = 0;
    std::atomic<int> saved = 0;
    auto save = [&](Document& doc) {
        ++ready;
        while (ready.load() < 2) std::this_thread::yield();
        try {
            doc.save(path);
            ++saved;
        } catch (const std::exception&) {}
    };
    std::thread worker(save, std::ref(a));
    save(b);
    worker.join();
    const auto result = contents(path);
    check(saved == 1 && (result == "first revision" || result == "second revision"),
          "one concurrent save should win");
}

void sparseFile(const fs::path& dir) {
    const auto path = dir / "huge";
    constexpr std::uint64_t tail = (std::uint64_t{5} << 30) + 17;
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    check(fd >= 0, "create sparse fixture");
    check(::pwrite(fd, "HEAD", 4, 0) == 4 && ::pwrite(fd, "TAIL", 4, tail) == 4, "sparse fixture");
    bool sparse_seeking = false;
#if defined(SEEK_DATA) && defined(SEEK_HOLE)
    sparse_seeking = ::lseek(fd, 4096 * 1024, SEEK_DATA) > static_cast<off_t>(tail / 2);
#endif
    ::close(fd);
    Document doc;
    doc.open(path);
    check(doc.size() == tail + 4 && doc.read(tail - 2, 100) == "\0\0TAIL"s, "64-bit seek");
    doc.replace(1, 1, "ello");
    doc.replace(tail + 3, 4, "END");
    check(doc.read(0, 7) == "HelloAD" && doc.read(tail + 2, 100) == "\0END"s, "64-bit edits");
    if (sparse_seeking) {
        doc.save(path);
        check(doc.size() == tail + 6 && doc.read(tail + 3, 10) == "END", "sparse save");
        struct stat info {};
        check(::stat(path.c_str(), &info) == 0 && info.st_blocks < (16 << 10), "preserved holes");
    }
    doc.replace(7, tail - 4, "");
    check(doc.read(0, 100) == "HelloADEND", "delete multi-GiB range");
    doc.save(path);
    check(contents(path) == "HelloADEND", "compact huge deletion");
}
} // namespace

int main() {
    try {
        Scratch box;
        basicEdits(box.path);
        randomEdits(box.path);
        largePaste(box.path);
        snapshots(box.path);
        conflicts(box.path);
        providerRevisions(box.path);
        concurrentSave(box.path);
        sparseFile(box.path);
        std::cout << "Document tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Document test failed: " << error.what() << '\n';
        return 1;
    }
}
