#pragma once

#include "document.hpp"

#include <glib.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace slate::view {

using Pos = std::uint64_t;

struct Row {
    Pos start, end, next;
    bool newline;
};

// Wrap extraordinarily long lines at predictable byte boundaries. After a random
// seek both enclosing boundaries can then be located without scanning the line.
class Rows {
public:
    explicit Rows(Document& doc) : doc_(doc) {}
    void invalidate() { cache_.clear(); }

    std::string_view bytes(Pos start, Pos count) {
        count = std::min(count, doc_.size() - start);
        if (start < origin_ || start - origin_ > cache_.size() ||
            count > cache_.size() - (start - origin_)) {
            origin_ = start / page * page;
            auto wanted = std::max<Pos>(2 * page, start - origin_ + count);
            cache_ = doc_.read(origin_, static_cast<std::size_t>(wanted));
        }
        return std::string_view(cache_).substr(start - origin_, count);
    }

    Pos start(Pos pos) {
        pos = std::min(pos, doc_.size());
        const Pos first = pos > 3 * span ? pos - 3 * span : 0;
        const Pos after = pos + std::min<Pos>(doc_.size() - pos, 5);
        auto vicinity = bytes(first, after - first);
        auto lf = vicinity.substr(0, pos - first).rfind('\n');
        Pos hard = lf == std::string_view::npos ? first : first + lf + 1;
        for (Pos boundary = pos / span * span; boundary >= hard && boundary - hard >= span;
             boundary -= span) {
            auto offset = align(vicinity, boundary - first);
            if (first + offset <= pos && soft(vicinity, offset))
                return first + offset;
        }
        return hard;
    }

    Row at(Pos pos) {
        const Pos first = pos > 4 ? pos - 4 : 0;
        auto vicinity = bytes(first, pos - first + 2 * span + 8);
        auto local = static_cast<std::size_t>(pos - first);
        auto tail = vicinity.substr(local);
        auto lf = tail.find('\n');
        bool hard_start = !pos || vicinity[local - 1] == '\n';
        Pos distance = span - pos % span;
        if (hard_start && distance < span) distance += span;
        if (distance < tail.size()) {
            auto offset = align(vicinity, local + distance);
            if (soft(vicinity, offset) && (lf == tail.npos || offset - local < lf)) {
                Pos next = first + offset;
                return {pos, next, next, false};
            }
        }
        if (lf != tail.npos) {
            Pos end = pos + lf;
            if (lf && tail[lf - 1] == '\r') --end;
            return {pos, end, pos + lf + 1, true};
        }
        return {pos, doc_.size(), doc_.size(), false};
    }

    bool last(const Row& row) const { return !row.newline && row.next == doc_.size(); }

    Pos left(Pos pos) {
        if (!pos) return 0;
        auto first = pos > 4 ? pos - 4 : 0;
        auto prev = bytes(first, pos - first);
        if (prev.ends_with("\r\n")) return pos - 2;
        for (std::size_t i = 0; i < prev.size(); ++i) {
            auto suffix = prev.substr(i);
            if (valid_length(suffix) == suffix.size()) return first + i;
        }
        return pos - 1;
    }

    Pos right(Pos pos) {
        if (pos >= doc_.size()) return doc_.size();
        auto next = bytes(pos, 4);
        if (next.starts_with("\r\n")) return pos + 2;
        return pos + valid_length(next);
    }

    static std::size_t valid_length(std::string_view input) {
        if (input.empty()) return 0;
        if (static_cast<unsigned char>(input.front()) < 128) return 1;
        auto ch = g_utf8_get_char_validated(input.data(), input.size());
        return ch == static_cast<gunichar>(-1) || ch == static_cast<gunichar>(-2)
            ? 1 : static_cast<std::size_t>(g_utf8_skip[static_cast<unsigned char>(input.front())]);
    }

private:
    static constexpr Pos span = 1024;
    static constexpr Pos page = 32768;
    Document& doc_;
    std::string cache_;
    Pos origin_ = 0;

    static std::size_t align(std::string_view text, std::size_t pos) {
        if (pos == text.size() || (static_cast<unsigned char>(text[pos]) & 0xc0) != 0x80)
            return pos;
        for (std::size_t back = 1; back <= pos && back <= 3; ++back) {
            auto length = valid_length(text.substr(pos - back));
            if (length > back) return pos - back + length;
        }
        return pos;
    }

    static bool soft(std::string_view text, std::size_t pos) {
        return pos < text.size() && text[pos] != '\n' && !text.substr(pos).starts_with("\r\n");
    }
};

} // namespace slate::view
