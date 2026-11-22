#include "markdown_model.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace slate;
using namespace std::string_literals;

void check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

MarkdownModel parse(std::string_view source) {
    auto model = parse_markdown(source).value();
    std::uint32_t end = 0, source_pos = 0;
    for (const auto& block : model.blocks) {
        check(block.begin == end && block.begin <= block.end, "block order");
        check(block.source_offset >= source_pos && block.source_offset <= source.size(), "source mapping");
        auto pos = block.begin;
        for (auto i = block.span_begin; i < block.span_end; ++i) {
            const auto& span = model.spans.at(i);
            check(span.begin >= pos && span.begin < span.end && span.end <= block.end, "span ranges");
            pos = span.end;
        }
        end = block.end;
        source_pos = block.source_offset;
    }
    check(end == model.text.size(), "text ranges cover output");
    return model;
}

std::string_view block_text(const MarkdownModel& model, std::size_t i) {
    const auto& block = model.blocks.at(i);
    return std::string_view(model.text).substr(block.begin, block.end - block.begin);
}

void expect_span(const MarkdownModel& m, std::size_t i, std::string_view content,
                 unsigned style, std::string_view destination = {}) {
    const auto& span = m.spans.at(i);
    check(std::string_view(m.text).substr(span.begin, span.end - span.begin) == content, "styled text");
    check(span.style == style, "inline formatting");
    if (span.destination != no_destination)
        check(m.destinations.at(span.destination) == destination, "target");
    else check(destination.empty(), "expected target");
}

void commonmark() {
    auto m = parse(R"md(Setext
======

*an **important** word* and `**plain**` with \*escaped\*.
Continues)md" "  \n" R"md(breaks.

A [named][REF] link and <https://example.org>.

[ref]: <docs/a&amp;b.md> "Title"

~~~cpp
auto x = "*literal*";

~~~

    indented

---

<p onclick="code()">raw HTML</p>

Inline <b>HTML</b>.
)md");
    check(m.blocks.size() == 8, "block count");
    check(m.blocks[0].kind == MarkdownKind::Heading && m.blocks[0].level == 1 &&
          block_text(m, 0) == "Setext", "setext heading");
    check(block_text(m, 1) == "an important word and **plain** with *escaped*. Continues\nbreaks.",
          "soft and hard breaks, escapes");
    expect_span(m, 0, "an ", MdEmphasis);
    expect_span(m, 1, "important", MdEmphasis | MdStrong);
    expect_span(m, 2, " word", MdEmphasis);
    expect_span(m, 3, "**plain**", MdCode);
    expect_span(m, 4, "named", MdLink, "docs/a&b.md");
    expect_span(m, 5, "https://example.org", MdLink, "https://example.org");
    check(m.blocks[3].kind == MarkdownKind::Code && block_text(m, 3) == "auto x = \"*literal*\";\n",
          "fenced code with blank line");
    check(m.blocks[4].kind == MarkdownKind::Code && block_text(m, 4) == "indented", "indented code");
    check(m.blocks[5].kind == MarkdownKind::Rule, "thematic break");
    check(m.blocks[6].kind == MarkdownKind::Html && block_text(m, 6) == "<p onclick=\"code()\">raw HTML</p>",
          "raw HTML displayed literally");
    check(block_text(m, 7) == "Inline <b>HTML</b>.", "inline HTML");
    check(m.blocks[1].source_offset == 15, "source line mapping");
}

void lists_and_tables() {
    auto m = parse(R"md(> 3) first
>    - [X] **done**
>    - [ ] ~~pending~~
> 4) next

- Start

  Second paragraph

- - nested

| Left | Center | Right |
| :--- | :----: | ----: |
| a    | b\|c   | **d** |
| only |
)md");
    check(m.blocks.size() == 11, "list and table block count");
    check(m.marker(m.blocks[0]) == "3)" && m.blocks[0].quote_depth == 1 && m.blocks[0].list_depth == 1,
          "ordered list in quote");
    check(m.marker(m.blocks[1]) == "☑" && m.blocks[1].list_depth == 2 && block_text(m, 1) == "done",
          "completed nested task");
    check(m.marker(m.blocks[2]) == "☐" && block_text(m, 2) == "pending", "unchecked task");
    expect_span(m, 0, "done", MdStrong);
    expect_span(m, 1, "pending", MdStrike);
    check(m.marker(m.blocks[3]) == "4)" && m.blocks[3].list_depth == 1, "outer list numbering resumes");
    check(m.marker(m.blocks[4]) == "•" && m.marker(m.blocks[5]).empty() && m.blocks[5].list_depth == 1,
          "loose list continuation");
    check(m.marker(m.blocks[6]) == "•" && block_text(m, 6).empty() && m.blocks[7].list_depth == 2,
          "empty parent list item");
    check(m.blocks[8].kind == MarkdownKind::TableHeader && block_text(m, 8) == "Left\tCenter\tRight", "table head");
    check(m.blocks[9].kind == MarkdownKind::TableRow && block_text(m, 9) == "a\tb|c\td" &&
          block_text(m, 10) == "only\t\t", "table rows and padding");
    const auto i = m.blocks[9].cell_begin;
    check(m.blocks[9].cell_end - i == 3 && m.cells[i].align == MarkdownAlign::Left &&
          m.cells[i + 1].align == MarkdownAlign::Center && m.cells[i + 2].align == MarkdownAlign::Right,
          "table alignment");
    const auto& middle = m.cells[i + 1];
    check(m.text.substr(middle.begin, middle.end - middle.begin) == "b|c", "cell boundaries");
    expect_span(m, 2, "d", MdStrong);
}

void images_and_entities() {
    auto m = parse("![*alt* &amp; ![nested](two)](pic&amp;.png) [![](icon)](target)\n\n"
                   "&NotEqualTilde; &#x1F600; &#233; &#0; &#xD800; &unknown;\n\n"
                   "Malformed \xff and NUL \0 text"s);
    expect_span(m, 0, "alt & nested", MdImage, "pic&.png");
    expect_span(m, 1, "\xef\xbf\xbc", MdImage | MdLink, "icon");
    check(m.destinations.at(m.spans[1].enclosing_link) == "target", "link wrapping image");
    check(block_text(m, 1) == "≂̸ 😀 é � � &unknown;", "numeric and named entities");
    check(block_text(m, 2) == "Malformed � and NUL � text", "valid display UTF-8");
}

void cancellation() {
    check(parse("").blocks.empty(), "empty document");
    check(!parse_markdown("never parsed", [] { return true; }), "early cancellation");
    std::string source;
    for (int i = 0; i < 256; ++i) source.append("**test**\n\n");
    unsigned polls = 0;
    check(!parse_markdown(source, [&] { return ++polls == 3; }), "cancel while rendering");
    const MarkdownLimits limits{2, 2, 2};
    check(!parse_markdown(source, {}, &limits), "discard model that exceeds block limit");
    check(!parse_markdown("*a* **b** ~~c~~", {}, &limits), "discard model that exceeds span limit");
    check(!parse_markdown("|a|b|\n|-|-|\n|c|d|", {}, &limits), "discard model that exceeds cell limit");
    check(parse_markdown("*a* **b**", {}, &limits).has_value(), "allow a model at the limit");
}
} // namespace

int main() {
    try {
        commonmark();
        lists_and_tables();
        images_and_entities();
        cancellation();
    } catch (const std::exception& error) {
        std::cerr << "Markdown model test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Markdown model tests passed\n";
}
