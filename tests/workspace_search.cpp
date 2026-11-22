#include "search.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace slate;

namespace {

void check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

struct TempDir {
    fs::path path;
    TempDir() {
        auto pattern = (fs::temp_directory_path() / "slate-search-XXXXXX").string();
        if (!mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
        path = pattern;
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

void write(const fs::path& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    check(out.good(), "write fixture");
}

std::vector<SearchHit> search(const fs::path& path, std::string_view query,
                              SearchMode mode = SearchMode::contents) {
    std::vector<SearchHit> hits;
    auto result = search_workspace(path, query, mode, 200, {}, [&](SearchHit h) {
        hits.push_back(std::move(h));
    });
    check(result.matches == hits.size() && !result.errors, "search completed without errors");
    return hits;
}

void boundary_matches() {
    TempDir temp;
    constexpr std::size_t block = 256 * 1024;
    const std::string needle = "every boundary";
    std::string content(2 * block, '.');
    content.replace(block - needle.size(), needle.size(), needle);
    content.replace(block, needle.size(), needle);
    content += std::string(needle);
    write(temp.path / "a.txt", content);
    auto hits = search(temp.path, needle);
    check(hits.size() == 3, "exactly three adjacent/edge matches");
    check(hits[0].offset == block - needle.size() && hits[1].offset == block &&
          hits[2].offset == 2 * block, "block edge byte offsets");
    for (std::size_t split = 1; split < needle.size(); ++split) {
        content.assign(block - split, 'a');
        content += needle;
        write(temp.path / "a.txt", content);
        hits = search(temp.path, needle);
        check(hits.size() == 1 && hits[0].offset == block - split &&
              hits[0].length == needle.size(), "split match position and size");
    }
    std::string long_needle(block + 7, 'a');
    long_needle.back() = 'X';
    write(temp.path / "a.txt", "zzz" + long_needle);
    hits = search(temp.path, long_needle);
    check(hits.size() == 1 && hits[0].offset == 3, "query larger than one chunk");
    write(temp.path / "a.txt", "ababa");
    hits = search(temp.path, "aba");
    check(hits.size() == 1 && hits[0].offset == 0, "non-overlapping matches");
    write(temp.path / "a.txt", "é\talpha <tag>\n");
    hits = search(temp.path, "<tag>");
    check(hits.size() == 1 && hits[0].offset == 9 && hits[0].excerpt == "é alpha <tag>",
          "UTF-8 byte offsets and one-line excerpt");
    hits = search(temp.path, "");
    check(hits.empty(), "empty query");
}

void sparse_offsets() {
    TempDir temp;
    auto file = temp.path / "big";
    constexpr std::uint64_t location = (std::uint64_t{5} << 30) + 37;
    std::ofstream out(file, std::ios::binary);
    out.seekp(location);
    out << "sparse-tail";
    out.close();
    check(fs::file_size(file) == location + 11, "sparse file size");
    const auto hits = search(temp.path, "sparse-tail");
    check(hits.size() == 1 && hits[0].offset == location && hits[0].length == 11,
          "64-bit sparse-file match");
    fs::remove(file);
    out.open(file, std::ios::binary);
    out.seekp(64 * 1024 - 1);
    out << 'Y';
    out.close();
    const auto binary = search(temp.path, std::string_view("\0Y", 2));
    check(binary.size() == 1 && binary[0].offset == 64 * 1024 - 2,
          "binary search needle can span a sparse hole");
}

void traversal_and_cancellation() {
    TempDir temp;
    auto nested = temp.path / "Src";
    fs::create_directory(nested);
    for (int i = 0; i < 1000; ++i) write(nested / ("Page-" + std::to_string(i)), "literal");
    fs::create_directory_symlink(temp.path, nested / "cycle");
    fs::create_symlink(nested / "Page-0", temp.path / "alias");
    auto hits = search(temp.path, "src/PAGE-999", SearchMode::filenames);
    check(hits.size() == 1 && hits[0].length == 0 && hits[0].path.filename() == "Page-999",
          "case-insensitive relative file name search");
    const auto summary = search_workspace(temp.path, "literal", SearchMode::contents, 1001, {},
                                          [](SearchHit) {});
    check(summary.matches == 1000 && !summary.reached_limit, "symlink cycles and aliases skipped");
    auto limited = search_workspace(temp.path, "Page", SearchMode::filenames, 5, {}, [](SearchHit) {});
    check(limited.matches == 5 && limited.reached_limit, "result limit");
    SearchCancellation stop;
    auto canceled = search_workspace(temp.path, "literal", SearchMode::contents, 100, stop,
                                     [&](SearchHit) { stop.request_stop(); });
    check(canceled.matches == 1 && !canceled.reached_limit, "cancel during traversal");
    auto early = search_workspace(temp.path, "x", SearchMode::contents, 5, stop, [](SearchHit) {});
    check(!early.matches && !early.errors, "already cancelled search");
    const auto missing = search_workspace(temp.path / "missing", "x", SearchMode::contents,
                                          5, {}, [](SearchHit) {});
    check(missing.errors == 1, "missing root is reported");
}

void text_search_in_recordings_folder() {
    TempDir temp;
    const std::string query = "marker";
    write(temp.path / "recording.WaV", query);
    write(temp.path / "recording.wav (2)", query);
    write(temp.path / "archive.sqlite3", query);
    write(temp.path / "unknown", query + '\0' + "binary");
    auto text = temp.path / "recording.wav.txt";
    constexpr std::size_t offset = 8190;
    write(text, std::string(offset, '.') + query);
    auto hits = search(temp.path, query, SearchMode::text_contents);
    check(hits.size() == 1 && hits.front().path == text && hits.front().offset == offset,
          "text search skips audio and binary files and preserves offsets across the probe");
    check(search(temp.path, query).size() == 5, "raw byte search includes binary files");
    check(search(temp.path, "wav", SearchMode::filenames).size() == 3,
          "recordings remain discoverable by name");
}

} // namespace

int main() {
    try {
        boundary_matches();
        sparse_offsets();
        traversal_and_cancellation();
        text_search_in_recordings_folder();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
