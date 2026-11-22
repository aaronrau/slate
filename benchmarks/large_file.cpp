#include "document.hpp"
#include "search.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <unistd.h>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

class Fixture {
public:
    Fixture() {
        auto pattern = (fs::temp_directory_path() / "slate-bench-XXXXXX").string();
        if (!mkdtemp(pattern.data())) throw std::runtime_error("Could not create benchmark directory");
        path = pattern;
    }
    ~Fixture() {
        std::error_code error;
        fs::remove_all(path, error);
    }
    fs::path path;
};

template<class F>
void measure(const char* name, F operation) {
    auto start = Clock::now();
    operation();
    auto ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    std::printf("%-42s %10.3f ms\n", name, ms);
}

void check(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

int main(int argc, char** argv) try {
    uint64_t gib = 1;
    if (argc > 1) {
        std::string_view arg(argv[1]);
        auto [end, err] = std::from_chars(arg.begin(), arg.end(), gib);
        check(argc == 2 && err == std::errc{} && end == arg.end() && gib > 0 &&
              gib <= (uint64_t{1} << 31), "Usage: slate_bench [positive size in GiB]");
    }
    Fixture temp;
    auto docs = temp.path / "text";
    auto entries = temp.path / "entries";
    fs::create_directories(docs);
    fs::create_directories(entries);
    auto file = docs / "large.md";
    constexpr size_t mib = 1024 * 1024;
    const uint64_t length = gib * 1024 * mib;
    check(fs::space(temp.path).available > 2 * length + 32 * mib,
          "Insufficient temporary disk space for benchmark input and saved copy");
    constexpr unsigned files = 50000;
    const std::string sentinel = "terminal-sentinel-1009";
    {
        std::ofstream out(file, std::ios::binary);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        std::string block(mib, 'a');
        const std::string record = "2026-01-01T12:03:19Z  INFO  worker=07  request completed in 8ms";
        for (size_t i = 0; i < block.size(); i += 64) {
            std::copy(record.begin(), record.end(), block.begin() + i);
            block[i + 18] = static_cast<char>('0' + (i / 64) % 10);
            block[i + 63] = '\n';
        }
        for (uint64_t offset = 0; offset < length; offset += mib) out.write(block.data(), block.size());
        out << sentinel;
        out.close();
    }
    for (unsigned i = 0; i < files; ++i) {
        std::ofstream entry(entries / ("note-" + std::to_string(i) + ".md"));
        check(entry.good(), "Failed to populate directory");
    }
    std::cout << "Generated " << gib << " GiB of dense text and 50,000 files. Warm filesystem cache.\n";
    slate::Document doc;
    measure("Open and read first 16 KiB", [&] {
        doc.open(file);
        check(doc.read(0, 16384).size() == 16384, "Failed first read");
    });
    measure("Read 2,000 random 4 KiB windows", [&] {
        uint64_t rng = 13579;
        for (int i = 0; i < 2000; ++i) {
            rng = rng * 6364136223846793005ULL + 1;
            check(doc.read(rng % (length - 4096), 4096).size() == 4096, "Failed random read");
        }
    });
    measure("Insert 10,000 bytes at scattered offsets", [&] {
        uint64_t rng = 1759;
        for (int i = 0; i < 10000; ++i) {
            rng = rng * 6364136223846793005ULL + 1;
            doc.replace(rng % length, 0, "X");
        }
        check(doc.size() == length + sentinel.size() + 10000, "Incorrect edited size");
    });
    measure("Search all 50,000 filenames", [&] {
        auto result = slate::search_workspace(entries, "note-43157.md", slate::SearchMode::filenames,
            10, {}, [](auto hit) { check(hit.path.filename() == "note-43157.md", "Wrong filename"); });
        check(result.matches == 1 && !result.errors, "Incomplete filename search");
    });
    measure("Find literal at end of dense file", [&] {
        auto result = slate::search_workspace(docs, sentinel, slate::SearchMode::contents,
            10, {}, [&](auto hit) { check(hit.offset == length, "Wrong match offset"); });
        check(result.matches == 1 && !result.errors, "Incomplete content search");
    });
    measure("Atomic save of edited dense file", [&] {
        doc.save(file);
        check(doc.read(doc.size() - sentinel.size(), sentinel.size()) == sentinel, "Lost tail");
    });
#ifdef __linux__
    // VmHWM covers this program image, excluding the launcher's pre-exec RSS.
    std::ifstream status("/proc/self/status");
    std::string field;
    while (std::getline(status, field)) {
        long kib;
        if (std::sscanf(field.c_str(), "VmHWM: %ld kB", &kib) == 1)
            std::printf("Peak RSS of benchmark process: %.1f MiB\n", kib / 1024.0);
    }
#else
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) == 0)
        std::printf("Peak RSS of benchmark process: %.1f MiB\n", usage.ru_maxrss / (1024.0 * 1024));
#endif
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
