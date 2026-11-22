#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace slate {

// Offsets and lengths are bytes. Opening a file reads only its metadata.
class Document {
public:
    Document();
    ~Document();
    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    void open(const std::filesystem::path& path);
    std::uint64_t size() const;
    std::string read(std::uint64_t offset, std::size_t count) const;
    void replace(std::uint64_t offset, std::uint64_t erase_length, std::string_view inserted);
    bool dirty() const;
    std::filesystem::path path() const;

    // Independent model sharing immutable text. A worker can save its own snapshot.
    Document snapshot() const;
    void save(const std::filesystem::path& path);

private:
    struct State;
    explicit Document(std::unique_ptr<State>);
    std::unique_ptr<State> state_;
};

} // namespace slate
