#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slate {

enum class MarkdownKind { Paragraph, Heading, Code, Rule, Html, TableHeader, TableRow, Mermaid };
enum MarkdownStyle : unsigned {
    MdEmphasis = 1, MdStrong = 2, MdCode = 4, MdStrike = 8, MdLink = 16, MdImage = 32
};
inline constexpr std::uint32_t no_destination = UINT32_MAX;

// Half-open byte offsets in MarkdownModel::text. Spans are sorted, disjoint,
// and cover styled text only. Image spans cover the full alt text, or U+FFFC.
struct MarkdownSpan {
    std::uint32_t begin, end;
    unsigned style;
    std::uint32_t destination = no_destination;
    std::uint32_t enclosing_link = no_destination; // Link surrounding an image.
};

enum class MarkdownAlign { Default, Left, Center, Right };
struct MarkdownCell {
    std::uint32_t begin, end;
    MarkdownAlign align;
};

struct MarkdownBlock {
    MarkdownKind kind = MarkdownKind::Paragraph;
    std::uint32_t begin = 0, end = 0;
    std::uint32_t span_begin = 0, span_end = 0;
    std::uint32_t cell_begin = 0, cell_end = 0;
    std::uint32_t source_offset = 0; // Approximate line start in parser input.
    unsigned level = 0, quote_depth = 0, list_depth = 0;
    std::uint32_t marker_begin = 0, marker_end = 0;
};

struct MarkdownModel {
    std::string text;
    std::string markers;
    std::vector<MarkdownBlock> blocks;
    std::vector<MarkdownSpan> spans;
    std::vector<MarkdownCell> cells;
    std::vector<std::string> destinations;

    std::string_view marker(const MarkdownBlock& block) const {
        return std::string_view(markers).substr(block.marker_begin, block.marker_end - block.marker_begin);
    }
};

struct MarkdownLimits {
    std::uint32_t blocks, spans, cells;
};

// Output is valid UTF-8. Cancellation or exceeding a supplied limit discards
// the partial model. Parser and allocation errors throw. Blocks share one
// buffer, with no separators added.
std::optional<MarkdownModel> parse_markdown(std::string_view input,
                                           const std::function<bool()>& cancelled = {},
                                           const MarkdownLimits* limits = nullptr);

} // namespace slate
