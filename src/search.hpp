#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace slate {

class SearchCancellation {
public:
    void request_stop() const { stopped_->store(true, std::memory_order_relaxed); }
    bool stop_requested() const { return stopped_->load(std::memory_order_relaxed); }

private:
    std::shared_ptr<std::atomic<bool>> stopped_ = std::make_shared<std::atomic<bool>>(false);
};

enum class SearchMode { filenames, contents };

struct SearchHit {
    std::filesystem::path path;
    std::uint64_t offset = 0;
    std::size_t length = 0;
    std::string excerpt;
};

struct SearchSummary {
    std::size_t matches = 0;
    std::uint64_t errors = 0;
    bool reached_limit = false;
};

using HitCallback = std::function<void(SearchHit)>;

// Filename searches match relative paths with ASCII case folding. Content searches
// match literal bytes and report non-overlapping byte ranges in regular files.
SearchSummary search_workspace(const std::filesystem::path& root, std::string_view query,
                               SearchMode mode, std::size_t limit, const SearchCancellation& stop,
                               const HitCallback& on_hit);

} // namespace slate
