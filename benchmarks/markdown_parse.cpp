#include "markdown_model.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

constexpr std::string_view prose = R"(A native editor can open a document without reading every byte. Each visible paragraph has enough text to wrap on a reasonably sized screen, with an occasional accent, café, or longer word. The rest of this paragraph exercises plain text and punctuation.
This sentence continues the same paragraph across a soft line break.

)";

constexpr std::string_view dense = R"(### Heading with *emphasis*

> 1. A **bold [link](https://example.org)** and `a + b`.
> 2. A task list follows.
>    - [x] Done
>    - [ ] ~~Incomplete~~ and &amp; more

| Left | Number |
| :--- | -----: |
| 42 | **three** |

![image](./picture.png)

~~~c
int answer = 42;
~~~

)";

enum class Mode { Full, PageAfterParse, PageDuringParse };

slate::MarkdownModel render(std::string_view source, Mode mode) {
    constexpr slate::MarkdownLimits limits{6000, 16000, 12000};
    auto model = slate::parse_markdown(source, {}, mode == Mode::PageDuringParse ? &limits : nullptr);
    if (mode == Mode::Full || (model && model->blocks.size() <= limits.blocks &&
            model->spans.size() <= limits.spans && model->cells.size() <= limits.cells))
        return std::move(*model);
    model.reset();
    return slate::parse_markdown(source.substr(0, 64 * 1024)).value();
}

void measure(std::string_view label, std::string_view pattern, std::size_t bytes, Mode mode = Mode::Full) {
    std::string source;
    source.reserve(bytes + pattern.size());
    while (source.size() < bytes) source.append(pattern);
    source.resize(bytes);

    std::vector<double> times;
    std::size_t blocks = 0, spans = 0;
    for (int i = 0; i <= 15; ++i) {
        const auto start = Clock::now();
        auto model = render(source, mode);
        const auto end = Clock::now();
        if (i) times.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        blocks = model.blocks.size();
        spans = model.spans.size();
    }
    std::sort(times.begin(), times.end());
    std::cout << label << '\t' << bytes / 1024 << '\t' << std::fixed << std::setprecision(3)
              << times[times.size() / 2] << '\t' << blocks << '\t' << spans << '\n';
}
} // namespace

int main() {
    std::cout << "fixture\tKiB\tmedian_ms\tblocks\tspans\n";
    for (auto bytes : {64u * 1024, 1024u * 1024}) {
        measure("prose", prose, bytes);
        measure("dense", dense, bytes);
    }
    measure("dense, page after full parse", dense, 1024 * 1024, Mode::PageAfterParse);
    measure("dense, page during parse", dense, 1024 * 1024, Mode::PageDuringParse);
}
