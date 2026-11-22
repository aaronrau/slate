#include "markdown_preview.hpp"
#include "markdown_model.hpp"
#include "mermaid_renderer.hpp"

#include <pango/pangocairo.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <utility>

namespace slate {
namespace {
namespace fs = std::filesystem;
using Offset = std::uint64_t;
constexpr Offset whole_limit = 1024 * 1024;
constexpr Offset page_size = 64 * 1024;
constexpr Offset dense_page_size = 8 * 1024;
constexpr unsigned none = no_destination;

struct Request {
    Document document;
    Offset page, offset;
    bool locate;
    Offset stride;
    std::uint64_t generation;
    std::function<void()> notify;
    int table_width;
    double dpi;
    int ascii_advance;
    std::function<void(unsigned, MermaidResult)> notify_diagram;
};

// Determine each boundary from its own small window so navigation works in
// both directions at any file offset without scanning preceding pages.
Offset boundary(const Document& document, Offset page, Offset stride) {
    if (page == 0) return 0;
    const auto nominal = page * stride;
    if (nominal >= document.size()) return document.size();
    const std::size_t window = stride / 32;
    const auto base = nominal - window;
    const auto sample = document.read(base, window + 1);
    Offset line = 0;
    for (std::size_t i = 0; i < window; ++i) {
        if (sample[i] == '\n') {
            if ((i && sample[i - 1] == '\n') ||
                (i >= 2 && sample[i - 1] == '\r' && sample[i - 2] == '\n'))
                line = base + i + 1;
        }
    }
    if (line) return line;
    const auto lf = sample.rfind('\n', window - 1);
    if (lf != std::string::npos) return base + lf + 1;
    std::size_t i = window;
    while (i > window - 3 && (static_cast<unsigned char>(sample[i]) & 0xc0) == 0x80) --i;
    return base + i;
}

std::string unescape(const std::string& uri) {
    gchar* decoded = g_uri_unescape_string(uri.c_str(), nullptr);
    if (!decoded) return uri;
    std::string result(decoded);
    g_free(decoded);
    return result;
}

struct LocalLink { fs::path path; std::string fragment; };
std::optional<LocalLink> local_link(const fs::path& document, const std::string& url) {
    auto hash = url.find('#');
    auto name = url.substr(0, hash);
    auto fragment = hash == url.npos ? std::string{} : unescape(url.substr(hash + 1));
    if (name.empty()) return LocalLink{document, fragment};
    if (name.starts_with("//")) return {};
    if (g_uri_peek_scheme(name.c_str())) {
        gchar* host = nullptr;
        gchar* filename = g_filename_from_uri(name.c_str(), &host, nullptr);
        bool local = !host || !*host || g_ascii_strcasecmp(host, "localhost") == 0;
        g_free(host);
        if (!filename) return {};
        fs::path path(filename);
        g_free(filename);
        if (local) return LocalLink{std::move(path), std::move(fragment)};
        return {};
    }
    return LocalLink{(document.parent_path() / unescape(name)).lexically_normal(), fragment};
}

using Pixbuf = std::shared_ptr<GdkPixbuf>;

// Probe dimensions before decoding. Static PNG/JPEG keep memory independent
// of frame counts, and the same bounded byte buffer backs probe and decode.
bool image_dimensions(const std::vector<guint8>& b, unsigned& width, unsigned& height) {
    if (b.size() >= 24 && !std::memcmp(b.data(), "\x89PNG\r\n\x1a\n", 8) &&
        !std::memcmp(b.data() + 12, "IHDR", 4)) {
        auto be32 = [&](std::size_t i) {
            return (unsigned(b[i]) << 24) | (unsigned(b[i+1]) << 16) |
                   (unsigned(b[i+2]) << 8) | b[i+3];
        };
        width = be32(16);
        height = be32(20);
        return true;
    }
    if (b.size() < 4 || b[0] != 0xff || b[1] != 0xd8) return false;
    auto be16 = [&](std::size_t i) { return (unsigned(b[i]) << 8) | b[i+1]; };
    std::size_t i = 2;
    while (i + 3 < b.size() && b[i] == 0xff) {
        while (i < b.size() && b[i] == 0xff) ++i;
        if (i + 2 >= b.size()) break;
        const unsigned marker = b[i++];
        const auto length = be16(i);
        if (length < 2 || length > b.size() - i) return false;
        if ((marker == 0xc0 || marker == 0xc1 || marker == 0xc2) && length >= 8) {
            height = be16(i + 3);
            width = be16(i + 5);
            return true;
        }
        if (marker == 0xda || marker == 0xd9) break;
        i += length;
    }
    return false;
}

Pixbuf load_image(const fs::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd == -1) return {};
    struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
    struct stat st {};
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > 4 * 1024 * 1024) return {};
    std::vector<guint8> bytes(st.st_size);
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        auto n = ::read(fd, bytes.data() + pos, bytes.size() - pos);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return {};
        pos += n;
    }
    unsigned width{}, height{};
    if (!image_dimensions(bytes, width, height) || !width || !height ||
        width > 16384 || height > 16384 || Offset(width) * height > 8 * 1024 * 1024) return {};
    const double scale = std::min({1., 640. / width, 360. / height});
    auto stream = g_memory_input_stream_new_from_data(bytes.data(), bytes.size(), nullptr);
    auto image = gdk_pixbuf_new_from_stream_at_scale(stream, std::max(1, int(width * scale)),
        std::max(1, int(height * scale)), TRUE, nullptr, nullptr);
    g_object_unref(stream);
    return Pixbuf(image, [](GdkPixbuf* p) { if (p) g_object_unref(p); });
}

std::string heading_id(std::string_view title) {
    std::string id;
    for (const char* p = title.data(); p < title.data() + title.size(); p = g_utf8_next_char(p)) {
        const auto c = g_utf8_get_char(p);
        if (g_unichar_isspace(c)) id += '-';
        else if (g_unichar_isalnum(c) || g_unichar_ismark(c) || c == '_' || c == '-') {
            char bytes[6];
            auto n = g_unichar_to_utf8(g_unichar_tolower(c), bytes);
            id.append(bytes, n);
        }
    }
    return id;
}

struct Format {
    MarkdownKind kind;
    unsigned level, depth, style;
    bool quoted;
    unsigned tabs = none;
    bool operator==(const Format&) const = default;
    bool same_block(const Format& other) const {
        return kind == other.kind && level == other.level && depth == other.depth &&
               quoted == other.quoted && tabs == other.tabs;
    }
};
struct Run {
    unsigned begin, end, image;
    int char_end;
    Format format;
    unsigned diagram = none;

    bool object() const { return image != none || diagram != none; }
};
struct Link { int begin, end; unsigned dest; };
struct TableTab {
    int x;
    MarkdownAlign align = MarkdownAlign::Left;
    bool operator==(const TableTab&) const = default;
};
struct Page {
    std::string text, error;
    std::vector<Run> runs;
    std::vector<Link> links;
    std::vector<std::string> destinations;
    std::unordered_map<std::string, int> anchors;
    std::vector<Pixbuf> images;
    std::vector<std::string> diagrams;
    std::vector<std::vector<TableTab>> tab_sets;
    int target = 0;
    Offset index = 0, count = 1, stride = 0;
};

constexpr double table_scale = .91;
constexpr int table_pitch = 24;
constexpr int cell_padding = 6;
constexpr int cell_minimum = 54;

void set_table_font(PangoLayout* layout, bool bold, bool italic = false) {
    auto* font = pango_font_description_from_string("monospace 12");
    pango_font_description_set_size(font, int(12 * table_scale * PANGO_SCALE));
    if (bold) pango_font_description_set_weight(font, PANGO_WEIGHT_BOLD);
    if (italic) pango_font_description_set_style(font, PANGO_STYLE_ITALIC);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
}

// Verify the actual proportional advance in the GTK context for all four
// inline style combinations. Font fallbacks and Unicode use Pango shaping.
int table_ascii_advance(PangoContext* context) {
    std::unique_ptr<PangoLayout, decltype(&g_object_unref)> layout(pango_layout_new(context), g_object_unref);
    std::array<char, 95> sample{};
    for (unsigned i = 0; i < sample.size(); ++i) sample[i] = char(i + 32);
    pango_layout_set_text(layout.get(), sample.data(), sample.size());
    int advance = 0;
    for (unsigned style = 0; style < 4; ++style) {
        set_table_font(layout.get(), style & 1, style & 2);
        auto* line = pango_layout_get_line_readonly(layout.get(), 0);
        for (unsigned i = 1; i <= sample.size(); ++i) {
            int x = 0;
            pango_layout_line_index_to_x(line, i, FALSE, &x);
            if (!advance) advance = x;
            if (x != int(i) * advance) return 0;
        }
    }
    return advance % PANGO_SCALE == 0 ? advance / PANGO_SCALE : 0;
}

std::size_t utf8_prefix(std::string_view text, std::size_t limit) {
    auto length = std::min(text.size(), limit);
    while (length < text.size() && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) --length;
    return length;
}

// Match the table's GtkTextTag typography on the worker. Reuse one layout
// for width measurement and cell wrapping, with no GTK widget per cell.
class TableText {
    std::unique_ptr<PangoLayout, decltype(&g_object_unref)> layout_{nullptr, g_object_unref};
    std::unordered_map<std::string_view, int> plain_widths_[2];
    std::string normalized_;
    double dpi_;
    int ascii_advance_;
    bool heading_ = false;

public:
    TableText(double dpi, int ascii_advance) : dpi_(dpi), ascii_advance_(ascii_advance) {}

    int ascii_advance(std::string_view text) const {
        return ascii_advance_ && std::all_of(text.begin(), text.end(), [](unsigned char c) { return c < 128; })
             ? ascii_advance_ : 0;
    }

    PangoLayout* layout(std::string_view text, unsigned begin, unsigned span,
                        const MarkdownModel& model, int width, bool heading) {
        if (!layout_) {
            auto* context = pango_font_map_create_context(pango_cairo_font_map_get_default());
            if (dpi_ > 0) pango_cairo_context_set_resolution(context, dpi_);
            pango_context_set_base_dir(context, PANGO_DIRECTION_LTR);
            layout_.reset(pango_layout_new(context));
            g_object_unref(context);
            pango_layout_set_auto_dir(layout_.get(), FALSE);
            pango_layout_set_wrap(layout_.get(), PANGO_WRAP_WORD_CHAR);
            heading_ = !heading;
        }
        if (heading_ != heading) {
            set_table_font(layout_.get(), heading);
            heading_ = heading;
        }
        PangoAttrList* attrs = nullptr;
        auto add = [&](PangoAttribute* attribute, const MarkdownSpan& s) {
            if (!attrs) attrs = pango_attr_list_new();
            attribute->start_index = std::max(s.begin, begin) - begin;
            attribute->end_index = std::min(s.end - begin, static_cast<unsigned>(text.size()));
            pango_attr_list_insert(attrs, attribute);
        };
        const auto end = begin + text.size();
        for (; span < model.spans.size() && model.spans[span].begin < end; ++span) {
            const auto& s = model.spans[span];
            if (s.end <= begin) continue;
            if ((s.style & MdStrong) && !heading) add(pango_attr_weight_new(PANGO_WEIGHT_BOLD), s);
            if (s.style & MdEmphasis) add(pango_attr_style_new(PANGO_STYLE_ITALIC), s);
        }
        pango_layout_set_attributes(layout_.get(), attrs);
        if (attrs) pango_attr_list_unref(attrs);
        pango_layout_set_width(layout_.get(), width < 0 ? -1 : width * PANGO_SCALE);
        if (text.find_first_of("\t\n\r") != text.npos) {
            normalized_.assign(text);
            for (auto& c : normalized_) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
            text = normalized_;
        }
        pango_layout_set_text(layout_.get(), text.data(), static_cast<int>(text.size()));
        return layout_.get();
    }

    int width(std::string_view text, unsigned begin, unsigned span, const MarkdownModel& model, bool heading) {
        if (text.empty()) return 0;
        if (const auto advance = ascii_advance(text)) return int(text.size()) * advance;
        const bool plain = text.size() <= 96 && (span == model.spans.size() ||
                           model.spans[span].begin >= begin + text.size());
        auto& cache = plain_widths_[heading];
        if (plain) if (auto it = cache.find(text); it != cache.end()) return it->second;
        int width = 0;
        pango_layout_get_pixel_size(layout(text, begin, span, model, -1, heading), &width, nullptr);
        if (plain && cache.size() < 256) cache.emplace(text, width);
        return width;
    }
};

struct Formatter {
    Page& page;
    const MarkdownModel& model;
    const fs::path path;
    const std::function<bool()>& cancelled;
    const Offset target;
    std::unordered_map<std::string_view, unsigned> image_cache;
    std::unordered_map<std::string, unsigned> duplicate_ids;
    unsigned line_chars = 0;
    unsigned max_line = page.stride == dense_page_size ? 384 : 4096;
    int chars = 0;
    int table_width;
    TableText table_text;
    std::unordered_multimap<std::size_t, unsigned> tab_indices;

    Formatter(Page& output, const MarkdownModel& input, fs::path document_path,
              const std::function<bool()>& cancel, Offset source_target, int width, double dpi, int advance)
        : page(output), model(input), path(std::move(document_path)), cancelled(cancel), target(source_target),
          table_width(width), table_text(dpi, advance) {}

    void run(std::string_view value, unsigned image, Format format, unsigned link, unsigned diagram = none) {
        auto begin = static_cast<unsigned>(page.text.size());
        page.text.append(value);
        auto end = static_cast<unsigned>(page.text.size());
        const bool object = image != none || diagram != none;
        if (begin == end && !object) return;
        const int next = chars + (object ? 1 : g_utf8_strlen(value.data(), value.size()));
        if (!object && !page.runs.empty() && !page.runs.back().object() &&
            page.runs.back().format == format) {
            page.runs.back().end = end;
            page.runs.back().char_end = next;
        }
        else page.runs.push_back({begin, end, image, next, format, diagram});
        if (link != none) {
            if (!page.links.empty() && page.links.back().dest == link && page.links.back().end == chars)
                page.links.back().end = next;
            else page.links.push_back({chars, next, link});
        }
        chars = next;
    }

    void text(std::string_view value, Format format, unsigned link = none) {
        if (format.tabs != none) {
            // Tabs and line breaks in a cell are text, not column separators.
            for (auto at = value.find_first_of("\t\n\r"); at != value.npos; at = value.find_first_of("\t\n\r")) {
                run(value.substr(0, at), none, format, link);
                run(" ", none, format, link);
                value.remove_prefix(at + 1);
            }
            run(value, none, format, link);
            return;
        }
        // Bound the work Pango has to do for a single pathological logical line.
        const auto end = value.data() + value.size();
        auto start = value.data();
        for (auto p = start; p < end; p = g_utf8_next_char(p)) {
            if (*p == '\n') line_chars = 0;
            else if (++line_chars > max_line) {
                run({start, static_cast<std::size_t>(p - start)}, none, format, link);
                run("\n", none, format, link);
                start = p;
                line_chars = 1;
            }
        }
        if (start < end) run({start, static_cast<std::size_t>(end - start)}, none, format, link);
    }

    unsigned image(unsigned dest) {
        if (dest >= model.destinations.size()) return none;
        auto& url = model.destinations[dest];
        if (auto it = image_cache.find(url); it != image_cache.end()) return it->second;
        if (image_cache.size() >= 24 || page.images.size() >= 12) return none;
        unsigned result = none;
        if (auto loc = local_link(path, model.destinations[dest]); loc && !loc->path.empty()) {
            if (auto pix = load_image(loc->path)) {
                result = page.images.size();
                page.images.push_back(std::move(pix));
            }
        }
        image_cache.emplace(url, result);
        return result;
    }

    void inlines(unsigned begin, unsigned end, unsigned& span, unsigned last, Format base) {
        auto source = std::string_view(model.text);
        while (begin < end) {
            while (span < last && model.spans[span].end <= begin) ++span;
            if (span == last || model.spans[span].begin >= end) {
                text(source.substr(begin, end - begin), base);
                return;
            }
            const auto& s = model.spans[span];
            if (s.begin > begin) text(source.substr(begin, s.begin - begin), base);
            begin = std::max(begin, s.begin);
            const unsigned limit = std::min(s.end, end);
            Format f = base;
            f.style = s.style;
            auto value = source.substr(begin, limit - begin);
            auto link = s.style & MdImage ? s.enclosing_link : (s.style & MdLink ? s.destination : none);
            if (s.style & MdImage) {
                auto index = image(s.destination);
                if (index != none) run({}, index, f, link);
                else text(value == "￼" ? std::string_view("Image") : value, f, link);
            } else text(value, f, link);
            begin = limit;
        }
    }

    unsigned table_tabs(const std::vector<int>& widths, unsigned first_cell) {
        std::vector<TableTab> tabs;
        tabs.reserve(widths.size() * 2 - 1);
        std::size_t hash = widths.size();
        int x = 0;
        auto append = [&](int at, MarkdownAlign align) {
            tabs.push_back({at, align});
            hash = (hash * 16777619u) ^ (unsigned(at) * 4 + unsigned(align));
        };
        for (unsigned c = 0; c < widths.size(); ++c) {
            if (c) append(x - table_pitch / 2 - 4, MarkdownAlign::Left);
            auto alignment = model.cells[first_cell + c].align;
            const auto w = widths[c];
            const auto offset = alignment == MarkdownAlign::Right ? w :
                                alignment == MarkdownAlign::Center ? w / 2 : 0;
            append(x + cell_padding + offset, alignment);
            x += w + table_pitch;
        }
        const auto [first, last] = tab_indices.equal_range(hash);
        for (auto it = first; it != last; ++it)
            if (page.tab_sets[it->second] == tabs) return it->second;
        auto id = static_cast<unsigned>(page.tab_sets.size());
        tab_indices.emplace(hash, id);
        page.tab_sets.push_back(std::move(tabs));
        return id;
    }

    Format block_format(const MarkdownBlock& block) const {
        return {block.kind, block.level, std::min(8u, block.list_depth + block.quote_depth), 0, block.quote_depth != 0};
    }

    std::size_t build_table(std::size_t start) {
        auto end = start + 1;
        while (end < model.blocks.size() && model.blocks[end].kind == MarkdownKind::TableRow) ++end;
        const auto& head = model.blocks[start];
        const unsigned columns = head.cell_end - head.cell_begin;
        const auto source = std::string_view(model.text);
        if (!columns) return end - 1;

        // Allocate width by shrinking the widest columns first. Empty or short
        // columns leave room for prose. A wide image and tables with many
        // columns can scroll horizontally.
        struct Measure { int width; unsigned span; bool has_image, long_text; };
        std::vector<Measure> measured;
        measured.reserve(model.blocks[end - 1].cell_end - head.cell_begin);
        std::vector<int> preferred(columns, cell_minimum), minimum = preferred;
        int available = std::max(180, table_width - int(block_format(head).depth * 18));
        const bool wrap = columns <= 12;
        for (auto i = start; i < end; ++i) {
            if (cancelled()) return end - 1;
            const auto& row = model.blocks[i];
            auto span = row.span_begin;
            for (auto j = row.cell_begin; j < row.cell_end; ++j) {
                const auto& c = model.cells[j];
                const auto col = j - row.cell_begin;
                while (span < row.span_end && model.spans[span].end <= c.begin) ++span;
                auto value = source.substr(c.begin, c.end - c.begin);
                // A cell this long will wrap regardless of the short sample's
                // advance. Bound the work in both measuring and folding it.
                const auto prefix = utf8_prefix(value, 1024);
                int width = table_text.width(value.substr(0, prefix), c.begin, span, model, i == start);
                const bool long_text = prefix < value.size();
                if (long_text) width = std::max(width, available);
                int image_width = 0;
                for (auto k = span; k < row.span_end && model.spans[k].begin < c.end; ++k) {
                    const auto& s = model.spans[k];
                    if (!(s.style & MdImage)) continue;
                    // The pixbuf replaces the alt text. Use an upper bound on
                    // mixed image/text cells so intrinsic sizes stay intact.
                    if (const auto index = image(s.destination); index != none)
                        image_width += gdk_pixbuf_get_width(page.images[index].get());
                }
                width += image_width + 2; // Rounding slack for different GTK/Pango pixel contexts.
                preferred[col] = std::max(preferred[col], width);
                if (image_width) minimum[col] = std::max(minimum[col], width);
                measured.push_back({width, span, image_width != 0, long_text});
            }
        }
        std::vector<int> widths = preferred;
        if (wrap) {
            const int budget = available - table_pitch * int(columns);
            int lower = cell_minimum, upper = std::max(cell_minimum, budget);
            while (lower < upper) {
                const int cap = (lower + upper + 1) / 2;
                int total = 0;
                for (unsigned c = 0; c < columns; ++c)
                    total += std::clamp(cap, minimum[c], preferred[c]);
                if (total <= budget) lower = cap;
                else upper = cap - 1;
            }
            for (unsigned c = 0; c < columns; ++c)
                widths[c] = std::clamp(lower, minimum[c], preferred[c]);
        }
        const auto tabs = table_tabs(widths, head.cell_begin);

        // A row has as many display lines as its tallest cell. Each display
        // line uses the same native tab stops and keeps inline tags and links.
        struct Slice { unsigned begin, end; };
        struct CellLines { std::size_t first, count; unsigned span; };
        std::vector<Slice> slices;
        std::vector<CellLines> cells;
        cells.reserve(columns);
        for (auto i = start; i < end; ++i) {
            if (cancelled()) break;
            const auto& row = model.blocks[i];
            if (target && row.source_offset <= target) page.target = chars;
            auto base = block_format(row);
            if (auto marker = model.marker(row); !marker.empty()) { text(marker, base); text("\n", base); }
            base.tabs = tabs;
            slices.clear();
            cells.clear();
            std::size_t height = 1;
            for (auto j = row.cell_begin; j < row.cell_end; ++j) {
                const auto& c = model.cells[j];
                const auto& size = measured[j - head.cell_begin];
                const auto padding = slices.size();
                const int width = widths[j - row.cell_begin];
                auto value = source.substr(c.begin, c.end - c.begin);
                if ((!size.long_text && size.width <= width) || size.has_image || value.empty()) {
                    slices.push_back({c.begin, c.end});
                } else if (const auto advance = table_text.ascii_advance(value)) {
                    const auto capacity = std::max(1, (width - 2) / advance);
                    auto begin = c.begin;
                    while (value.size() > std::size_t(capacity)) {
                        if (cancelled()) return end - 1;
                        auto length = value.substr(0, capacity).find_last_of(" \t\r\n");
                        length = length == value.npos ? capacity : length + 1;
                        slices.push_back({begin, begin + unsigned(length)});
                        begin += length;
                        value.remove_prefix(length);
                    }
                    slices.push_back({begin, c.end});
                } else {
                    auto span = size.span;
                    for (unsigned offset = c.begin; offset < c.end;) {
                        if (cancelled()) return end - 1;
                        auto remaining = source.substr(offset, c.end - offset);
                        const auto length = utf8_prefix(remaining, 4096);
                        while (span < row.span_end && model.spans[span].end <= offset) ++span;
                        auto* layout = table_text.layout(remaining.substr(0, length), offset, span, model,
                                                        width - 2, i == start);
                        for (auto* list = pango_layout_get_lines_readonly(layout); list; list = list->next) {
                            auto* line = static_cast<const PangoLayoutLine*>(list->data);
                            const auto first = offset + static_cast<unsigned>(line->start_index);
                            slices.push_back({first, first + static_cast<unsigned>(line->length)});
                        }
                        offset += length;
                    }
                }
                cells.push_back({padding, slices.size() - padding, size.span});
                height = std::max(height, cells.back().count);
            }
            for (std::size_t line = 0; line < height; ++line) {
                for (unsigned j = 0; j < cells.size(); ++j) {
                    run(j ? "\t│\t" : "\t", none, base, none);
                    auto& cell = cells[j];
                    if (line < cell.count) {
                        auto range = slices[cell.first + line];
                        inlines(range.begin, range.end, cell.span, row.span_end, base);
                    }
                }
                if (line + 1 < height) run("\xe2\x80\xa8", none, base, none); // Pango soft line break.
                else if (i + 1 < model.blocks.size()) run("\n", none, base, none);
            }
            line_chars = 0;
        }
        return end - 1;
    }

    bool diagram(std::string_view value, Format base) {
        if (value.size() > 16384 || diagram_instances >= 12) return false;
        auto it = std::find(page.diagrams.begin(), page.diagrams.end(), value);
        const auto index = static_cast<unsigned>(it - page.diagrams.begin());
        if (it == page.diagrams.end()) {
            if (index >= 8) return false;
            page.diagrams.emplace_back(value);
        }
        ++diagram_instances;
        run({}, none, base, none, index);
        return true;
    }

    unsigned diagram_instances = 0;

    void build() {
        for (std::size_t i = 0; i < model.blocks.size(); ++i) {
            if (cancelled()) return;
            auto& b = model.blocks[i];
            if (b.kind == MarkdownKind::TableHeader) {
                i = build_table(i);
                continue;
            }
            if (target && b.source_offset <= target) page.target = chars;
            auto base = block_format(b);
            if (b.kind == MarkdownKind::Heading) {
                const auto slug = heading_id(std::string_view(model.text).substr(b.begin, b.end - b.begin));
                auto id = slug;
                auto& count = duplicate_ids[slug];
                while (page.anchors.contains(id)) id = slug + "-" + std::to_string(++count);
                page.anchors.emplace(std::move(id), chars);
            }
            if (auto marker = model.marker(b); !marker.empty()) { text(marker, base); text("  ", base); }
            if (b.kind == MarkdownKind::Rule) text("━━━━━━━━━━━━━━━━━━━━", base);
            unsigned span = b.span_begin;
            if (b.kind == MarkdownKind::Mermaid) {
                if (!diagram(std::string_view(model.text).substr(b.begin, b.end - b.begin), base)) {
                    base.kind = MarkdownKind::Code;
                    text("Mermaid preview limit reached.\n", base);
                    inlines(b.begin, b.end, span, b.span_end, base);
                }
            } else inlines(b.begin, b.end, span, b.span_end, base);
            if (i + 1 < model.blocks.size()) text("\n", base);
        }
    }
};

Page prepare(const Request& req, const std::function<bool()>& cancelled) {
    Page result;
    const auto size = req.document.size();
    const auto offset = req.locate ? std::min(req.offset, size ? size - 1 : 0)
                                   : boundary(req.document, req.page, req.stride);
    Offset stride = req.stride;
    if (!stride && size > dense_page_size) {
        // A pathological line is expensive to wrap even when little source is
        // visible. Sample at the requested position; page navigation keeps the
        // chosen stride so every byte remains reachable in either direction.
        const auto base = (offset / page_size) * page_size;
        const auto sample = req.document.read(base, page_size);
        std::size_t column = 0, lines = 0;
        for (char c : sample) {
            if (g_ascii_isspace(c)) { column = 0; if (c == '\n') ++lines; }
            else if (++column > 8192) { stride = dense_page_size; break; }
        }
        if (lines > 8000) stride = dense_page_size;
    }
    if (!stride && size > whole_limit) stride = page_size;
    Offset start = 0;
    for (;;) {
        Offset end = size;
        if (stride) {
            result.stride = stride;
            result.count = std::max<Offset>(1, (size + stride - 1) / stride);
            auto i = offset / stride;
            if (i + 1 < result.count && offset >= boundary(req.document, i + 1, stride)) ++i;
            result.index = std::min(i, result.count - 1);
            start = boundary(req.document, result.index, stride);
            end = boundary(req.document, result.index + 1, stride);
        }
        auto source = req.document.read(start, end - start);
        constexpr MarkdownLimits limits{6000, 16000, 12000};
        auto model = parse_markdown(source, cancelled, &limits);
        if (cancelled()) return {};
        if (!model) {
            // Short Markdown table rows can expand to the full header width.
            // Bound that expansion even on inputs already in paged mode.
            stride = !stride && size > page_size ? page_size :
                     (!stride || stride > dense_page_size ? dense_page_size : stride / 4);
            continue;
        }
        Formatter{result, *model, req.document.path(), cancelled, req.locate && offset >= start ? offset - start : 0,
                  req.table_width, req.dpi, req.ascii_advance}.build();
        result.destinations = std::move(model->destinations);
        return result;
    }
}

// Exactly one worker per active preview. Rapid edits overwrite the pending
// snapshot. Workers own no widgets and can finish after window destruction.
class Worker : public std::enable_shared_from_this<Worker> {
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Request> next_;
    std::optional<Page> ready_;
    std::atomic<std::uint64_t> generation_{0};
    std::optional<MermaidRenderer> mermaid_;
    bool running_ = false;
    bool close_browser_ = false;

    static void notify(std::function<void()> callback) {
        auto* task = new std::function<void()>(std::move(callback));
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer data) -> gboolean {
            (*static_cast<std::function<void()>*>(data))();
            return G_SOURCE_REMOVE;
        }, task, +[](gpointer data) { delete static_cast<std::function<void()>*>(data); });
    }

    void process() {
        for (;;) {
            std::optional<Request> current;
            {
                std::unique_lock lock(mutex_);
                if (!next_ && mermaid_ && mermaid_->browser_running() && !close_browser_) {
                    wake_.wait_for(lock, std::chrono::milliseconds(1500), [&] { return next_ || close_browser_; });
                }
                if (mermaid_ && mermaid_->browser_running() && (!next_ || close_browser_)) {
                    close_browser_ = false;
                    lock.unlock();
                    mermaid_->release_browser();
                    continue;
                }
                if (!next_) { running_ = false; return; }
                close_browser_ = false;
                current.swap(next_);
            }
            auto cancelled = [&] { return generation_.load() != current->generation; };
            Page result;
            try { result = prepare(*current, cancelled); }
            catch (const std::exception& e) { result.error = e.what(); }
            const auto diagrams = result.diagrams;
            {
                std::lock_guard lock(mutex_);
                if (cancelled()) continue;
                ready_ = std::move(result);
            }
            notify(std::move(current->notify));
            if (!diagrams.empty() && !mermaid_) mermaid_.emplace();
            for (unsigned index = 0; index < diagrams.size() && !cancelled(); ++index) {
                MermaidResult rendered;
                try { rendered = mermaid_->render(diagrams[index], cancelled); }
                catch (const std::exception&) { mermaid_->release_browser(); }
                if (cancelled()) break;
                notify([callback = current->notify_diagram, index, rendered = std::move(rendered)]() mutable {
                    callback(index, std::move(rendered));
                });
            }
        }
    }
public:
    void cancel() {
        std::lock_guard lock(mutex_);
        ++generation_;
        next_.reset();
        ready_.reset();
        close_browser_ = true;
        wake_.notify_one();
    }
    void submit(Request request) {
        std::lock_guard lock(mutex_);
        request.generation = ++generation_;
        next_ = std::move(request);
        ready_.reset();
        wake_.notify_one();
        if (!running_) {
            std::thread t([self = shared_from_this()] { self->process(); });
            running_ = true;
            t.detach();
        }
    }
    std::optional<Page> take() {
        std::optional<Page> result;
        std::lock_guard lock(mutex_);
        ready_.swap(result);
        return result;
    }
};
} // namespace

struct MarkdownPreview::Impl {
    GtkWidget *box, *bar, *status, *previous, *next, *view;
    GtkTextTagTable* table = gtk_text_tag_table_new();
    std::array<GtkTextTag*, 7> headings{};
    std::array<GtkTextTag*, 9> indents{};
    std::vector<GtkTextTag*> row_tabs;
    guint reflow_timer = 0;
    int rendered_width = 0;
    guint table_font_serial = 0;
    int ascii_advance = 0;
    std::optional<int> reflow_position;
    GtkTextTag *paragraph, *code, *html, *rule, *table_row, *table_head, *quote;
    GtkTextTag *emphasis, *strong, *inline_code, *strike, *link, *alt;
    std::shared_ptr<Worker> worker = std::make_shared<Worker>();
    std::optional<Document> document;
    std::optional<Page> page;
    std::string fragment;
    std::function<void(const fs::path&, const std::string&)> open_link;
    guint source = 0;
    std::size_t run_index = 0;
    unsigned run_byte = 0;
    std::uint64_t diagram_generation = 0;
    std::vector<std::optional<MermaidResult>> diagram_results;
    std::vector<std::vector<GtkWidget*>> diagram_widgets;
    // Only the main thread dereferences this token. A worker may already have
    // queued its GLib notification when the window closes.
    std::shared_ptr<Impl*> receiver = std::make_shared<Impl*>(this);
    bool destroyed = false;

    Impl() {
        box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        g_object_ref_sink(box);
        bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(bar), 5);
        gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
        previous = gtk_button_new_with_label("Previous page");
        next = gtk_button_new_with_label("Next page");
        gtk_widget_set_name(previous, "markdown-previous");
        gtk_widget_set_name(next, "markdown-next");
        status = gtk_label_new("");
        gtk_widget_set_name(status, "markdown-status");
        gtk_label_set_xalign(GTK_LABEL(status), 0);
        gtk_label_set_ellipsize(GTK_LABEL(status), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(bar), status, TRUE, TRUE, 6);
        gtk_box_pack_start(GTK_BOX(bar), previous, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bar), next, FALSE, FALSE, 0);
        gtk_widget_set_no_show_all(bar, TRUE);
        gtk_widget_show(status);
        auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
        auto* buffer = gtk_text_buffer_new(table);
        view = gtk_text_view_new_with_buffer(buffer);
        gtk_widget_set_name(view, "markdown-text");
        g_signal_connect(view, "size-allocate", G_CALLBACK(+[](GtkWidget*, GtkAllocation*, Impl* self) {
            self->schedule_table_reflow();
        }), this);
        gtk_container_add(GTK_CONTAINER(scroll), view);
        auto* text_view = GTK_TEXT_VIEW(view);
        gtk_text_view_set_wrap_mode(text_view, GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_editable(text_view, FALSE);
        gtk_text_view_set_cursor_visible(text_view, FALSE);
        gtk_text_view_set_left_margin(text_view, 28);
        gtk_text_view_set_right_margin(text_view, 28);
        gtk_text_view_set_top_margin(text_view, 16);
        gtk_text_view_set_bottom_margin(text_view, 24);
        auto css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css,
            "textview, textview text { background-color: #1d2329; color: #dce3e6; font: 12pt sans-serif; }"
            "textview text selection { color: white; background-color: #406c76; }", -1, nullptr);
        gtk_style_context_add_provider(gtk_widget_get_style_context(view), GTK_STYLE_PROVIDER(css),
                                       GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
        paragraph = gtk_text_buffer_create_tag(buffer, "md-paragraph", "pixels-above-lines", 3,
                                               "pixels-below-lines", 5, nullptr);
        double scales[] = {1, 1.85, 1.5, 1.28, 1.13, 1., .95};
        for (unsigned i = 1; i < headings.size(); ++i) {
            auto name = "md-h" + std::to_string(i);
            headings[i] = gtk_text_buffer_create_tag(buffer, name.c_str(), "scale", scales[i],
                "weight", PANGO_WEIGHT_BOLD, "pixels-above-lines", 10, "pixels-below-lines", 5, nullptr);
        }
        for (unsigned i = 1; i < indents.size(); ++i) {
            indents[i] = gtk_text_buffer_create_tag(buffer, nullptr, "left-margin", int(28 + 18 * i), nullptr);
        }
        quote = gtk_text_buffer_create_tag(buffer, "md-quote", "foreground", "#a1bbc5",
                                            "paragraph-background", "#232e36", nullptr);
        code = gtk_text_buffer_create_tag(buffer, "md-code-block", "family", "monospace",
            "scale", .93, "paragraph-background", "#151a20", "foreground", "#dbd5bd", nullptr);
        html = gtk_text_buffer_create_tag(buffer, "md-html", "family", "monospace",
                                          "foreground", "#b0b9c2", nullptr);
        rule = gtk_text_buffer_create_tag(buffer, "md-rule", "foreground", "#66757a", nullptr);
        table_row = gtk_text_buffer_create_tag(buffer, "md-cell", "family", "monospace", "scale", table_scale,
            "paragraph-background", "#222b32", "wrap-mode", GTK_WRAP_NONE, "direction", GTK_TEXT_DIR_LTR,
            "pixels-above-lines", 1, "pixels-below-lines", 1, nullptr);
        table_head = gtk_text_buffer_create_tag(buffer, "md-table-head", "weight", PANGO_WEIGHT_BOLD,
            "foreground", "#b0dccc", "paragraph-background", "#2c373e", "pixels-below-lines", 3, nullptr);
        emphasis = gtk_text_buffer_create_tag(buffer, "md-em", "style", PANGO_STYLE_ITALIC, nullptr);
        strong = gtk_text_buffer_create_tag(buffer, "md-strong", "weight", PANGO_WEIGHT_BOLD, nullptr);
        inline_code = gtk_text_buffer_create_tag(buffer, "md-code", "family", "monospace",
                                                 "background", "#303941", "foreground", "#efddb7", nullptr);
        strike = gtk_text_buffer_create_tag(buffer, "md-strike", "strikethrough", TRUE, nullptr);
        alt = gtk_text_buffer_create_tag(buffer, "md-alt", "style", PANGO_STYLE_ITALIC, nullptr);
        link = gtk_text_buffer_create_tag(buffer, "md-link", "underline", PANGO_UNDERLINE_SINGLE,
                                          "foreground", "#87daca", nullptr);
        g_object_unref(buffer);

        g_signal_connect(box, "destroy", G_CALLBACK(+[](GtkWidget*, Impl* self) {
            self->destroyed = true;
            *self->receiver = nullptr;
            self->stop();
        }), this);
        g_signal_connect(previous, "clicked", G_CALLBACK(+[](GtkButton*, Impl* self) {
            if (self->page && self->page->index > 0) self->start(self->page->index - 1, 0, false);
        }), this);
        g_signal_connect(next, "clicked", G_CALLBACK(+[](GtkButton*, Impl* self) {
            if (self->page && self->page->index + 1 < self->page->count)
                self->start(self->page->index + 1, 0, false);
        }), this);
        gtk_widget_add_events(view, GDK_POINTER_MOTION_MASK);
        g_signal_connect(view, "motion-notify-event", G_CALLBACK(+[](GtkWidget*, GdkEventMotion* e,
                                                                     Impl* self) -> gboolean {
            auto* window = gtk_text_view_get_window(GTK_TEXT_VIEW(self->view), GTK_TEXT_WINDOW_TEXT);
            if (window && e->window == window) {
                bool hot = self->pointer_link(e->window, e->x, e->y) != none;
                auto* cursor = gdk_cursor_new_from_name(gtk_widget_get_display(self->view), hot ? "pointer" : "text");
                gdk_window_set_cursor(window, cursor);
                if (cursor) g_object_unref(cursor);
            }
            return FALSE;
        }), this);
        g_signal_connect(view, "event-after", G_CALLBACK(+[](GtkWidget*, GdkEvent* e, Impl* self) {
            if (e->type == GDK_BUTTON_PRESS)
                gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(self->view), FALSE);
            if (e->type != GDK_BUTTON_RELEASE || e->button.button != 1 ||
                gtk_text_buffer_get_has_selection(self->buffer())) return;
            auto dest = self->pointer_link(e->button.window, e->button.x, e->button.y);
            if (dest != none) self->activate(dest);
        }), this);
        g_signal_connect(view, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* e,
                                                                  Impl* self) -> gboolean {
            if (e->keyval >= GDK_KEY_Home && e->keyval <= GDK_KEY_End)
                gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(self->view), TRUE);
            if (e->keyval != GDK_KEY_Return && e->keyval != GDK_KEY_KP_Enter) return FALSE;
            GtkTextIter where;
            gtk_text_buffer_get_iter_at_mark(self->buffer(), &where, gtk_text_buffer_get_insert(self->buffer()));
            auto dest = self->at(gtk_text_iter_get_offset(&where));
            if (dest == none) return FALSE;
            self->activate(dest);
            return TRUE;
        }), this);
    }

    ~Impl() {
        stop();
        if (!destroyed) gtk_widget_destroy(box);
        g_object_unref(box);
        g_object_unref(table);
    }

    GtkTextBuffer* buffer() const { return gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)); }

    void cancel_reflow() {
        if (reflow_timer) { g_source_remove(reflow_timer); reflow_timer = 0; }
        reflow_position.reset();
    }

    int available_width() const {
        const int allocated = gtk_widget_get_allocated_width(view);
        return allocated > 1 ? std::max(180, allocated - 72) : 760;
    }

    void schedule_table_reflow() {
        if (!page || row_tabs.empty() || !g_object_get_data(G_OBJECT(box), "markdown-ready")) return;
        if (reflow_timer) { g_source_remove(reflow_timer); reflow_timer = 0; }
        if (std::abs(available_width() - rendered_width) < 8) return;
        // Let a window or sidebar drag settle before reshaping table cells.
        reflow_timer = g_timeout_add(100, +[](gpointer data) -> gboolean {
            auto* self = static_cast<Impl*>(data);
            self->reflow_timer = 0;
            GdkRectangle visible;
            auto* text = GTK_TEXT_VIEW(self->view);
            gtk_text_view_get_visible_rect(text, &visible);
            GtkTextIter top;
            gtk_text_view_get_line_at_y(text, &top, visible.y, nullptr);
            const auto position = gtk_text_iter_get_offset(&top);
            self->start(self->page->index, 0, false);
            self->reflow_position = position;
            return G_SOURCE_REMOVE;
        }, this);
    }

    void stop() {
        cancel_reflow();
        if (source) { g_source_remove(source); source = 0; }
        ++diagram_generation;
        diagram_widgets.clear();
        diagram_results.clear();
        worker->cancel();
        document.reset();
        page.reset();
    }

    void reset_buffer() {
        auto* blank = gtk_text_buffer_new(table);
        gtk_text_view_set_buffer(GTK_TEXT_VIEW(view), blank);
        g_object_unref(blank);
        for (auto* tag : row_tabs) if (tag) gtk_text_tag_table_remove(table, tag);
        row_tabs.clear();
        g_object_set_data(G_OBJECT(box), "markdown-ready", nullptr);
    }

    void info(const std::string& message) {
        gtk_widget_show(bar);
        gtk_label_set_text(GTK_LABEL(status), message.c_str());
        gtk_widget_set_tooltip_text(status, message.c_str());
    }

    void start(Offset index, Offset offset, bool locate) {
        cancel_reflow();
        const Offset stride = !locate && page ? page->stride : 0;
        if (source) { g_source_remove(source); source = 0; }
        page.reset();
        run_index = 0;
        run_byte = 0;
        const auto revision = ++diagram_generation;
        diagram_widgets.clear();
        diagram_results.clear();
        gtk_widget_hide(previous);
        gtk_widget_hide(next);
        info("Loading preview…");
        reset_buffer();
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
        auto* font_context = gtk_widget_get_pango_context(view);
        const auto dpi = pango_cairo_context_get_resolution(font_context);
        if (const guint serial = pango_context_get_serial(font_context); serial != table_font_serial) {
            table_font_serial = serial;
            ascii_advance = table_ascii_advance(font_context);
        }
        rendered_width = available_width();
        worker->submit({document->snapshot(), index, offset, locate, stride, 0, [token = receiver, revision] {
            auto* self = *token;
            if (!self || self->diagram_generation != revision) return;
            auto result = self->worker->take();
            if (!result) return;
            self->page = std::move(result);
            self->row_tabs.resize(self->page->tab_sets.size());
            self->diagram_widgets.resize(self->page->diagrams.size());
            self->diagram_results.resize(self->page->diagrams.size());
            if (!self->batch()) { self->finish(); return; }
            self->source = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer data) -> gboolean {
                auto* self = static_cast<Impl*>(data);
                if (self->batch()) return G_SOURCE_CONTINUE;
                self->source = 0;
                self->finish();
                return G_SOURCE_REMOVE;
            }, self, nullptr);
        }, rendered_width, dpi, ascii_advance, [token = receiver, revision](unsigned index, MermaidResult result) {
            auto* self = *token;
            if (!self || self->diagram_generation != revision) return;
            if (index >= self->diagram_results.size()) self->diagram_results.resize(index + 1);
            self->diagram_results[index] = std::move(result);
            if (index < self->diagram_widgets.size())
                for (auto* widget : self->diagram_widgets[index]) self->present_diagram(index, widget);
        }});
    }

    void present_diagram(unsigned index, GtkWidget* box) {
        auto* child = gtk_bin_get_child(GTK_BIN(box));
        if (child) gtk_widget_destroy(child);
        const auto& result = diagram_results[index];
        if (!result) {
            child = gtk_label_new("Rendering Mermaid diagram…");
        } else if (result->image) {
            child = gtk_image_new_from_pixbuf(result->image.get());
            gtk_widget_set_name(child, "markdown-mermaid-image");
            atk_object_set_name(gtk_widget_get_accessible(child), "Mermaid diagram");
        } else {
            child = gtk_expander_new(result->status == MermaidStatus::Invalid
                ? "Could not render Mermaid diagram" : "Mermaid needs a Chromium browser");
            auto* label = gtk_label_new(page->diagrams[index].c_str());
            gtk_label_set_selectable(GTK_LABEL(label), TRUE);
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
            gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
            gtk_label_set_max_width_chars(GTK_LABEL(label), 65);
            gtk_container_add(GTK_CONTAINER(child), label);
            gtk_widget_show(label);
        }
        gtk_container_add(GTK_CONTAINER(box), child);
        gtk_widget_show(child);
    }

    GtkTextTag* table_tab_tag(unsigned index) {
        auto*& tag = row_tabs[index];
        if (tag) return tag;
        const auto& stops = page->tab_sets[index];
        auto* tabs = pango_tab_array_new(static_cast<int>(stops.size()), TRUE);
        for (unsigned i = 0; i < stops.size(); ++i) {
            auto align = PANGO_TAB_LEFT;
            if (stops[i].align == MarkdownAlign::Center) align = PANGO_TAB_CENTER;
            if (stops[i].align == MarkdownAlign::Right) align = PANGO_TAB_RIGHT;
            pango_tab_array_set_tab(tabs, i, align, stops[i].x);
        }
        tag = gtk_text_buffer_create_tag(buffer(), nullptr, "tabs", tabs, nullptr);
        pango_tab_array_free(tabs);
        return tag;
    }

    void apply_block(Format f, GtkTextIter& begin, GtkTextIter& end) {
        auto add = [&](GtkTextTag* tag) { gtk_text_buffer_apply_tag(buffer(), tag, &begin, &end); };
        switch (f.kind) {
        case MarkdownKind::Heading: add(headings[std::clamp(f.level, 1u, 6u)]); break;
        case MarkdownKind::Code: add(code); break;
        case MarkdownKind::Html: add(html); break;
        case MarkdownKind::Rule: add(rule); break;
        case MarkdownKind::TableHeader: add(table_row); add(table_head); break;
        case MarkdownKind::TableRow: add(table_row); break;
        default: add(paragraph); break;
        }
        if (f.depth) add(indents[f.depth]);
        if (f.quoted) add(quote);
        if (f.tabs != none) add(table_tab_tag(f.tabs));
    }

    void apply_inline(Format f, GtkTextIter& begin, GtkTextIter& end) {
        auto add = [&](GtkTextTag* tag) { gtk_text_buffer_apply_tag(buffer(), tag, &begin, &end); };
        if (f.style & MdStrong) add(strong);
        if (f.style & MdEmphasis) add(emphasis);
        if (f.style & MdStrike) add(strike);
        if (f.style & MdCode) add(inline_code);
        if (f.style & MdLink) add(link);
        if (f.style & MdImage) add(alt);
    }

    bool batch() {
        const auto deadline = g_get_monotonic_time() + 3000;
        const auto& runs = page->runs;
        unsigned inserted = 0;
        while (run_index < runs.size()) {
            const auto& first_run = runs[run_index];
            GtkTextIter end;
            gtk_text_buffer_get_end_iter(buffer(), &end);
            const auto pos = gtk_text_iter_get_offset(&end);
            if (first_run.object()) {
                if (first_run.diagram != none) {
                    // One child anchor occupies the same character offset
                    // before and after rendering. Links and heading jumps
                    // therefore remain valid while diagrams arrive.
                    auto* anchor = gtk_text_buffer_create_child_anchor(buffer(), &end);
                    auto* widget = gtk_event_box_new();
                    gtk_event_box_set_visible_window(GTK_EVENT_BOX(widget), FALSE);
                    gtk_widget_set_name(widget, "markdown-mermaid");
                    gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(view), widget, anchor);
                    diagram_widgets[first_run.diagram].push_back(widget);
                    present_diagram(first_run.diagram, widget);
                    gtk_widget_show(widget);
                } else {
                gtk_text_buffer_insert_pixbuf(buffer(), &end, page->images[first_run.image].get());
                }
                GtkTextIter begin;
                gtk_text_buffer_get_iter_at_offset(buffer(), &begin, pos);
                apply_block(first_run.format, begin, end);
                apply_inline(first_run.format, begin, end);
                ++run_index;
            } else {
                const unsigned start = std::max(run_byte, first_run.begin);
                auto last = run_index;
                while (last + 1 < runs.size() && !runs[last + 1].object() &&
                       last - run_index < 96 && runs[last].end - start < 8192) ++last;
                auto limit = std::min(runs[last].end, start + 8192);
                while (limit < runs[last].end && (static_cast<unsigned char>(page->text[limit]) & 0xc0) == 0x80) --limit;
                gtk_text_buffer_insert(buffer(), &end, page->text.data() + start, limit - start);
                inserted += limit - start;
                const auto final_pos = gtk_text_iter_get_offset(&end);
                GtkTextIter begin;
                gtk_text_buffer_get_iter_at_offset(buffer(), &begin, pos);
                auto block_begin = begin;
                for (;;) {
                    const auto& r = runs[run_index];
                    const auto stop_pos = std::min(r.char_end, final_pos);
                    GtkTextIter stop = begin;
                    gtk_text_iter_forward_chars(&stop, stop_pos - gtk_text_iter_get_offset(&begin));
                    if (r.format.style) apply_inline(r.format, begin, stop);
                    const bool done = stop_pos == final_pos;
                    auto base = r.format;
                    if (done || !base.same_block(runs[run_index + 1].format)) {
                        apply_block(base, block_begin, stop);
                        block_begin = stop;
                    }
                    if (stop_pos == r.char_end) { ++run_index; run_byte = 0; }
                    else run_byte = limit;
                    if (done) break;
                    begin = stop;
                }
            }
            if (inserted >= 32768 || g_get_monotonic_time() >= deadline) break;
        }
        return run_index < runs.size();
    }

    void scroll(int position) {
        GtkTextIter iter;
        gtk_text_buffer_get_iter_at_offset(buffer(), &iter, position);
        gtk_text_buffer_place_cursor(buffer(), &iter);
        if (position) {
            gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(view), gtk_text_buffer_get_insert(buffer()), 0, TRUE, 0, 0);
        } else {
            auto* adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view));
            if (adjustment) gtk_adjustment_set_value(adjustment, gtk_adjustment_get_lower(adjustment));
        }
    }

    void finish() {
        if (!page->error.empty()) info(page->error);
        else if (page->count > 1) {
            info("Page " + std::to_string(page->index + 1) + " of " + std::to_string(page->count) +
                 "  ·  Markdown is parsed per page");
            gtk_widget_set_tooltip_text(status, "Lists, fenced code, references and heading links can span source pages. "
                                                "Each page is rendered independently.");
            gtk_widget_show(previous);
            gtk_widget_show(next);
            gtk_widget_set_sensitive(previous, page->index > 0);
            gtk_widget_set_sensitive(next, page->index + 1 < page->count);
        } else gtk_widget_hide(bar);
        if (!fragment.empty()) {
            auto anchor = page->anchors.find(fragment);
            if (anchor != page->anchors.end()) scroll(anchor->second);
            else { scroll(page->target); info("Heading not found on this page"); }
            fragment.clear();
        } else if (reflow_position) {
            scroll(std::min(*reflow_position, gtk_text_buffer_get_char_count(buffer())));
            reflow_position.reset();
        } else scroll(page->target);
        std::vector<Run>().swap(page->runs);
        std::vector<Pixbuf>().swap(page->images);
        std::vector<std::vector<TableTab>>().swap(page->tab_sets);
        std::string().swap(page->text);
        g_object_set_data(G_OBJECT(box), "markdown-ready", GINT_TO_POINTER(1));
        schedule_table_reflow();
    }

    unsigned at(int pos) const {
        if (!page) return none;
        const auto& links = page->links;
        auto it = std::lower_bound(links.begin(), links.end(), pos,
            [](const Link& l, int p) { return l.end <= p; });
        return it != links.end() && it->begin <= pos ? it->dest : none;
    }

    unsigned pointer_link(GdkWindow* window, double x, double y) const {
        int bx, by, trailing;
        auto* v = GTK_TEXT_VIEW(view);
        if (gtk_text_view_get_window(v, GTK_TEXT_WINDOW_TEXT) != window) return none;
        gtk_text_view_window_to_buffer_coords(v, GTK_TEXT_WINDOW_TEXT, int(x), int(y), &bx, &by);
        GtkTextIter where;
        if (!gtk_text_view_get_iter_at_position(v, &where, &trailing, bx, by)) return none;
        return at(gtk_text_iter_get_offset(&where));
    }

    void activate(unsigned dest) {
        if (!document || !page || dest >= page->destinations.size()) return;
        std::string url = page->destinations[dest];
        if (url.empty()) return;
        try {
            if (auto local = local_link(document->path(), url)) {
                if (url[0] == '#' || (!document->path().empty() && local->path == document->path())) {
                    if (local->fragment.empty()) scroll(0);
                    else if (auto it = page->anchors.find(local->fragment); it != page->anchors.end())
                        scroll(it->second);
                    else info("Heading not found on this page");
                    return;
                }
                auto extension = local->path.extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](char c) { return g_ascii_tolower(c); });
                if (open_link && (extension == ".md" || extension == ".markdown")) {
                    open_link(local->path, local->fragment);
                    return;
                }
                auto absolute = fs::absolute(local->path);
                auto* uri = g_filename_to_uri(absolute.c_str(), nullptr, nullptr);
                if (!uri) { info("Unable to open this file path"); return; }
                url = uri;
                g_free(uri);
                auto hash = page->destinations[dest].find('#');
                if (hash != url.npos) url += page->destinations[dest].substr(hash);
            } else {
                auto* scheme = g_uri_peek_scheme(url.c_str());
                if (!scheme || (g_ascii_strcasecmp(scheme, "https") && g_ascii_strcasecmp(scheme, "http") &&
                                g_ascii_strcasecmp(scheme, "mailto"))) {
                    info("Unsupported link address");
                    return;
                }
            }
            auto top = gtk_widget_get_toplevel(box);
            GError* error = nullptr;
            gtk_show_uri_on_window(GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr, url.c_str(),
                                   gtk_get_current_event_time(), &error);
            if (error) { info(error->message); g_error_free(error); }
        } catch (const std::exception& e) { info(e.what()); }
    }
};

MarkdownPreview::MarkdownPreview() : impl_(std::make_unique<Impl>()) {}
MarkdownPreview::~MarkdownPreview() = default;
GtkWidget* MarkdownPreview::widget() const { return impl_->box; }
void MarkdownPreview::show(Document snapshot, Offset offset, std::string fragment) {
    if (impl_->destroyed) return;
    impl_->document = std::move(snapshot);
    impl_->fragment = std::move(fragment);
    impl_->start(0, offset, true);
}
void MarkdownPreview::clear() {
    impl_->stop();
    if (!impl_->destroyed) { impl_->reset_buffer(); gtk_widget_hide(impl_->bar); }
}
void MarkdownPreview::focus() {
    if (!impl_->destroyed) gtk_widget_grab_focus(impl_->view);
}
void MarkdownPreview::set_open_link(std::function<void(const fs::path&, const std::string&)> open) {
    impl_->open_link = std::move(open);
}

} // namespace slate
