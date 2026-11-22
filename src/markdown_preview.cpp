#include "markdown_preview.hpp"
#include "markdown_model.hpp"
#include "mermaid_renderer.hpp"
#include "theme.hpp"

#include <pango/pangocairo.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
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
#include <variant>

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
    std::function<void()> notify;
    int table_width;
    double dpi;
};

struct DiagramRequest {
    std::vector<std::string> sources;
    Palette palette;
    std::function<void(unsigned, MermaidResult)> notify;
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
    std::uint8_t level, depth, style;
    bool quoted;
    unsigned tabs = none;
    bool last_table_row = false;
    bool operator==(const Format&) const = default;
    bool same_block(const Format& other) const {
        return kind == other.kind && level == other.level && depth == other.depth &&
               quoted == other.quoted && tabs == other.tabs && last_table_row == other.last_table_row;
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
struct TableColumn {
    int stop, right;
    MarkdownAlign align = MarkdownAlign::Left;
    bool operator==(const TableColumn&) const = default;
};
struct TableRow {
    int begin, end;
    unsigned columns;
    bool header, stripe, last;
};
struct Page {
    std::string text, error;
    std::vector<Run> runs;
    std::vector<Link> links;
    std::vector<std::string> destinations;
    std::unordered_map<std::string, int> anchors;
    std::vector<Pixbuf> images;
    std::vector<std::string> diagrams;
    std::vector<std::vector<TableColumn>> table_columns;
    std::vector<TableRow> table_rows;
    int target = 0, table_fit_width = 0;
    Offset index = 0, count = 1, stride = 0;
};

constexpr int cell_padding = 10;
constexpr int table_pitch = 2 * cell_padding;
constexpr int cell_minimum = 28;
constexpr int row_padding = 6;
constexpr int table_spacing = 5;
constexpr char table_font[] = "Sans 11";

std::size_t utf8_prefix(std::string_view text, std::size_t limit) {
    auto length = std::min(text.size(), limit);
    while (length < text.size() && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) --length;
    return length;
}

// Cell content can include tabs and Unicode line breaks through character
// references. Preserve byte offsets for the model's inline spans.
std::string_view cell_text(std::string_view text, std::string& normalized) {
    normalized.clear();
    for (auto at = text.find_first_of("\t\n\v\f\r\xc2\xe2"); at != text.npos;
         at = text.find_first_of("\t\n\v\f\r\xc2\xe2", at + 1)) {
        std::string_view space;
        if (static_cast<unsigned char>(text[at]) < 128) space = " ";
        else if (text.substr(at, 2) == "\xc2\x85") space = "\xc2\xa0";
        else if (text.substr(at, 3) == "\xe2\x80\xa8" || text.substr(at, 3) == "\xe2\x80\xa9")
            space = "\xe2\x80\x85";
        if (!space.empty()) {
            if (normalized.empty()) normalized.assign(text);
            normalized.replace(at, space.size(), space);
        }
    }
    return normalized.empty() ? text : std::string_view(normalized);
}

bool starts_right_to_left(std::string_view text) {
    if (std::none_of(text.begin(), text.end(), [](unsigned char c) { return c >= 128; })) return false;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    return pango_find_base_dir(text.data(), static_cast<int>(text.size())) == PANGO_DIRECTION_RTL;
    G_GNUC_END_IGNORE_DEPRECATIONS
}

// Match the table's GtkTextTag typography on the worker. Reuse one layout
// for width measurement and cell wrapping, with no GTK widget per cell.
struct CellSlice { unsigned begin, end; };

class TableText {
    std::unique_ptr<PangoLayout, decltype(&g_object_unref)> layout_{nullptr, g_object_unref};
    using StyleRange = std::array<unsigned, 3>;
    struct Metrics {
        std::vector<StyleRange> styles;
        int natural = -1, wrapped = -1;
        std::vector<CellSlice> lines;
    };
    std::unordered_map<std::string_view, Metrics> cache_[2];
    std::vector<StyleRange> styles_;
    std::string normalized_;
    double dpi_;
    bool heading_ = false;

    // Repeated labels and formatted descriptions are common in large tables.
    // Compare the exact shaping attributes as well as the text, so identical
    // words with different emphasis or code fonts keep their own metrics.
    Metrics* metrics(std::string_view text, unsigned begin, unsigned span, const MarkdownModel& model, bool heading) {
        if (text.size() > 384) return nullptr;
        auto& cache = cache_[heading];
        auto found = cache.find(text);
        if (found == cache.end() && cache.size() >= 512) return nullptr;
        styles_.clear();
        const unsigned end = begin + text.size();
        for (; span < model.spans.size() && model.spans[span].begin < end; ++span) {
            const auto& s = model.spans[span];
            const auto style = s.style & (MdStrong | MdEmphasis | MdCode | MdImage);
            if (style && s.end > begin)
                styles_.push_back({std::max(s.begin, begin) - begin, std::min(s.end, end) - begin, style});
        }
        if (found == cache.end()) return &cache.emplace(text, Metrics{styles_}).first->second;
        return found->second.styles == styles_ ? &found->second : nullptr;
    }

public:
    explicit TableText(double dpi) : dpi_(dpi) {}

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
            auto* font = pango_font_description_from_string(table_font);
            if (heading) pango_font_description_set_weight(font, PANGO_WEIGHT_BOLD);
            pango_layout_set_font_description(layout_.get(), font);
            pango_font_description_free(font);
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
            if (s.style & (MdEmphasis | MdImage)) add(pango_attr_style_new(PANGO_STYLE_ITALIC), s);
            if (s.style & MdCode) add(pango_attr_family_new("monospace"), s);
        }
        pango_layout_set_attributes(layout_.get(), attrs);
        if (attrs) pango_attr_list_unref(attrs);
        pango_layout_set_width(layout_.get(), width < 0 ? -1 : width * PANGO_SCALE);
        text = cell_text(text, normalized_);
        pango_layout_set_text(layout_.get(), text.data(), static_cast<int>(text.size()));
        return layout_.get();
    }

    int width(std::string_view text, unsigned begin, unsigned span, const MarkdownModel& model, bool heading) {
        if (text.empty()) return 0;
        auto* cached = metrics(text, begin, span, model, heading);
        if (cached && cached->natural >= 0) return cached->natural;
        int width = 0;
        pango_layout_get_pixel_size(layout(text, begin, span, model, -1, heading), &width, nullptr);
        if (cached) cached->natural = width;
        return width;
    }

    void wrap(std::string_view text, unsigned begin, unsigned span, const MarkdownModel& model,
              int width, bool heading, std::vector<CellSlice>& slices) {
        auto* cached = metrics(text, begin, span, model, heading);
        if (cached && cached->wrapped == width) {
            for (const auto& line : cached->lines) slices.push_back({begin + line.begin, begin + line.end});
            return;
        }
        if (cached) { cached->wrapped = width; cached->lines.clear(); }
        auto* shaped = layout(text, begin, span, model, width, heading);
        for (auto* list = pango_layout_get_lines_readonly(shaped); list; list = list->next) {
            auto* line = static_cast<const PangoLayoutLine*>(list->data);
            const auto first = static_cast<unsigned>(line->start_index);
            const auto last = first + static_cast<unsigned>(line->length);
            slices.push_back({begin + first, begin + last});
            if (cached) cached->lines.push_back({first, last});
        }
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
    std::string normalized_cell;
    std::unordered_multimap<std::size_t, unsigned> tab_indices;
    std::size_t display_cells = 0;
    bool table_limit = false;

    Formatter(Page& output, const MarkdownModel& input, fs::path document_path,
              const std::function<bool()>& cancel, Offset source_target, int width, double dpi)
        : page(output), model(input), path(std::move(document_path)), cancelled(cancel), target(source_target),
          table_width(width), table_text(dpi) {}

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
            run(cell_text(value, normalized_cell), none, format, link);
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
        std::vector<TableColumn> columns;
        columns.reserve(widths.size());
        std::size_t hash = widths.size();
        int x = 0;
        for (unsigned c = 0; c < widths.size(); ++c) {
            const auto alignment = model.cells[first_cell + c].align;
            const int width = widths[c];
            const int offset = alignment == MarkdownAlign::Right ? width :
                               alignment == MarkdownAlign::Center ? width / 2 : 0;
            const int stop = x + cell_padding + offset;
            x += width + table_pitch;
            columns.push_back({stop, x, alignment});
            hash = (hash * 16777619u) ^ (unsigned(width) * 4 + unsigned(alignment));
        }
        const auto [first, last] = tab_indices.equal_range(hash);
        for (auto it = first; it != last; ++it)
            if (page.table_columns[it->second] == columns) return it->second;
        auto id = static_cast<unsigned>(page.table_columns.size());
        tab_indices.emplace(hash, id);
        page.table_columns.push_back(std::move(columns));
        return id;
    }

    Format block_format(const MarkdownBlock& block) const {
        const auto depth = std::min(8u, block.list_depth + block.quote_depth);
        return {block.kind, std::uint8_t(block.level), std::uint8_t(depth), 0, block.quote_depth != 0};
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
                int image_width = 0, widest_image = 0;
                for (auto k = span; k < row.span_end && model.spans[k].begin < c.end; ++k) {
                    const auto& s = model.spans[k];
                    if (!(s.style & MdImage)) continue;
                    if (const auto index = image(s.destination); index != none) {
                        const int pixels = gdk_pixbuf_get_width(page.images[index].get());
                        image_width += pixels;
                        widest_image = std::max(widest_image, pixels);
                        const auto alt_end = std::min<unsigned>(s.end, c.begin + prefix);
                        if (alt_end > s.begin)
                            width -= table_text.width(source.substr(s.begin, alt_end - s.begin), s.begin, k, model, i == start);
                    }
                }
                width = std::max(0, width) + image_width + 2; // GTK/Pango rounding slack.
                preferred[col] = std::max(preferred[col], width);
                // Leave prose enough room for several words even when the
                // viewport is narrow. Tables wider than this can be scrolled.
                minimum[col] = std::max({minimum[col], widest_image + 2, std::min(width, 104)});
                measured.push_back({width, span, image_width != 0, long_text});
            }
        }
        std::vector<int> widths = preferred;
        int natural = (table_pitch * int(columns)) + int(block_format(head).depth * 18);
        for (auto width : preferred) natural += width;
        page.table_fit_width = std::max(page.table_fit_width, natural);
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
        const auto tabs = table_tabs(widths, head.cell_begin);
        if (page.table_columns[tabs].back().right > 65536) {
            table_limit = true;
            return end - 1;
        }

        // A row has as many display lines as its tallest cell. Each display
        // line uses the same native tab stops and keeps inline tags and links.
        struct CellLines { std::size_t first, count; unsigned span; };
        std::vector<CellSlice> slices;
        std::vector<CellLines> cells;
        cells.reserve(columns);
        for (auto i = start; i < end; ++i) {
            if (cancelled()) break;
            const auto& row = model.blocks[i];
            if (target && row.source_offset <= target) page.target = chars;
            auto base = block_format(row);
            if (auto marker = model.marker(row); !marker.empty()) { text(marker, base); text("\n", base); }
            base.tabs = tabs;
            base.last_table_row = i + 1 == end;
            const int row_begin = chars;
            slices.clear();
            cells.clear();
            std::size_t height = 1;
            for (auto j = row.cell_begin; j < row.cell_end; ++j) {
                const auto& c = model.cells[j];
                const auto& size = measured[j - head.cell_begin];
                const auto padding = slices.size();
                const int width = widths[j - row.cell_begin];
                auto value = source.substr(c.begin, c.end - c.begin);
                if ((!size.long_text && size.width <= width) || value.empty()) {
                    slices.push_back({c.begin, c.end});
                } else {
                    auto span = size.span;
                    auto wrap_text = [&](unsigned offset, unsigned end) {
                        while (offset < end && !cancelled()) {
                            auto remaining = source.substr(offset, end - offset);
                            const auto length = utf8_prefix(remaining, 4096);
                            while (span < row.span_end && model.spans[span].end <= offset) ++span;
                            table_text.wrap(remaining.substr(0, length), offset, span, model,
                                            width - 2, i == start, slices);
                            offset += length;
                        }
                    };
                    // A decoded image is one unbreakable object in the view,
                    // irrespective of the length or shaping of its alt text.
                    auto begin = c.begin;
                    if (size.has_image) {
                        for (auto k = span; k < row.span_end && model.spans[k].begin < c.end; ++k) {
                            const auto& item = model.spans[k];
                            if (!(item.style & MdImage) || image(item.destination) == none) continue;
                            auto stop = item.begin;
                            while (stop > begin && source[stop - 1] == ' ') --stop;
                            wrap_text(begin, stop);
                            slices.push_back({item.begin, item.end});
                            begin = item.end;
                            while (begin < c.end && source[begin] == ' ') ++begin;
                        }
                    }
                    wrap_text(begin, c.end);
                    if (cancelled()) return end - 1;
                }
                cells.push_back({padding, slices.size() - padding, size.span});
                height = std::max(height, cells.back().count);
            }
            display_cells += height * columns;
            // A pathological cell or many padded continuations would
            // otherwise create an oversized GTK paragraph or display page.
            if (height * columns > 8192 || display_cells > 48000) {
                table_limit = true;
                return end - 1;
            }
            // Keep Markdown column order when a row's first strong character
            // is RTL. Pango still applies bidirectional shaping inside cells.
            if (starts_right_to_left(source.substr(row.begin, row.end - row.begin)))
                run("\xe2\x80\x8e", none, base, none); // LRM.
            for (std::size_t line = 0; line < height; ++line) {
                for (unsigned j = 0; j < cells.size(); ++j) {
                    run("\t", none, base, none);
                    auto& cell = cells[j];
                    if (line < cell.count) {
                        auto range = slices[cell.first + line];
                        inlines(range.begin, range.end, cell.span, row.span_end, base);
                    }
                }
                // Close the last tab field before the soft break. Pango
                // otherwise includes the break in center/right alignment.
                run("\t", none, base, none);
                if (line + 1 < height) run("\xe2\x80\xa8", none, base, none);
                else if (i + 1 < model.blocks.size()) run("\n", none, base, none);
            }
            page.table_rows.push_back({row_begin, chars, tabs, i == start, (i - start) % 2 == 0, base.last_table_row});
            line_chars = 0;
        }
        return end - 1;
    }

    bool diagram(std::string_view value, Format base) {
        if (value.size() > max_mermaid_source || diagram_instances >= 12) return false;
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

    bool build() {
        for (std::size_t i = 0; i < model.blocks.size(); ++i) {
            if (cancelled()) return false;
            auto& b = model.blocks[i];
            if (b.kind == MarkdownKind::TableHeader) {
                i = build_table(i);
                if (table_limit) return false;
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
        return true;
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
        if (model && Formatter{result, *model, req.document.path(), cancelled,
                               req.locate && offset >= start ? offset - start : 0,
                               req.table_width, req.dpi}.build()) {
            result.destinations = std::move(model->destinations);
            return result;
        }
        if (cancelled()) return {};
        // Tables expand to the full header width and may wrap each row.
        // Bound both the parsed and displayed page on dense input.
        stride = !stride && size > page_size ? page_size :
                 (!stride || stride > dense_page_size ? dense_page_size : stride / 4);
        result = Page{};
    }
}

// Exactly one worker per active preview. Rapid edits overwrite the pending
// snapshot. Workers own no widgets and can finish after window destruction.
class Worker : public std::enable_shared_from_this<Worker> {
    using Job = std::variant<Request, DiagramRequest>;
    std::mutex mutex_;
    std::optional<Job> next_;
    std::optional<Page> ready_;
    std::atomic<std::uint64_t> generation_{0};
    std::optional<MermaidRenderer> mermaid_;
    bool running_ = false;

    static void notify(std::function<void()> callback) {
        auto* task = new std::function<void()>(std::move(callback));
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer data) -> gboolean {
            (*static_cast<std::function<void()>*>(data))();
            return G_SOURCE_REMOVE;
        }, task, +[](gpointer data) { delete static_cast<std::function<void()>*>(data); });
    }

    void render_diagrams(const DiagramRequest& job, const std::function<bool()>& cancelled) {
        if (!mermaid_) mermaid_.emplace();
        for (unsigned index = 0; index < job.sources.size() && !cancelled(); ++index) {
            MermaidResult rendered;
            try { rendered = mermaid_->render(job.sources[index], job.palette, cancelled); }
            catch (const std::exception&) {}
            if (cancelled()) break;
            notify([callback = job.notify, index, rendered = std::move(rendered)]() mutable {
                callback(index, std::move(rendered));
            });
        }
    }

    void process() {
        for (;;) {
            std::optional<Job> current;
            std::uint64_t revision;
            {
                std::lock_guard lock(mutex_);
                if (!next_) { running_ = false; return; }
                current.swap(next_);
                revision = generation_.load();
            }
            auto cancelled = [&] { return generation_.load() != revision; };
            if (const auto* diagrams = std::get_if<DiagramRequest>(&*current)) {
                render_diagrams(*diagrams, cancelled);
                continue;
            }
            auto& request = std::get<Request>(*current);
            Page result;
            try { result = prepare(request, cancelled); }
            catch (const std::exception& e) { result.error = e.what(); }
            {
                std::lock_guard lock(mutex_);
                if (cancelled()) continue;
                ready_ = std::move(result);
            }
            notify(std::move(request.notify));
        }
    }
public:
    void cancel() {
        std::lock_guard lock(mutex_);
        ++generation_;
        next_.reset();
        ready_.reset();
    }
    void submit(Job request) {
        std::lock_guard lock(mutex_);
        ++generation_;
        next_ = std::move(request);
        ready_.reset();
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
// GtkTextView draws this layer in buffer coordinates, after its CSS background
// and before native selections, text, links and inline images.
struct PreviewTextView {
    GtkTextView parent;
    void (*draw_tables)(void*, cairo_t*);
    void* owner;
};
struct PreviewTextViewClass { GtkTextViewClass parent; };
G_DEFINE_TYPE(PreviewTextView, preview_text_view, GTK_TYPE_TEXT_VIEW)

void preview_text_view_init(PreviewTextView*) {}

void preview_text_view_class_init(PreviewTextViewClass* klass) {
    GTK_TEXT_VIEW_CLASS(klass)->draw_layer = +[](GtkTextView* view, GtkTextViewLayer layer, cairo_t* cr) {
        auto* parent = GTK_TEXT_VIEW_CLASS(preview_text_view_parent_class);
        if (parent->draw_layer) parent->draw_layer(view, layer, cr);
        auto* preview = reinterpret_cast<PreviewTextView*>(view);
        if (layer == GTK_TEXT_VIEW_LAYER_BELOW_TEXT && preview->draw_tables)
            preview->draw_tables(preview->owner, cr);
    };
}
} // namespace

struct MarkdownPreview::Impl {
    GtkWidget *box, *bar, *status, *previous, *next, *view;
    Palette palette = default_palette(true);
    GtkCssProvider* css = gtk_css_provider_new();
    GtkTextTagTable* table = gtk_text_tag_table_new();
    std::array<GtkTextTag*, 7> headings{};
    std::array<GtkTextTag*, 9> indents{};
    std::vector<GtkTextTag*> row_tabs;
    guint reflow_timer = 0;
    int rendered_width = 0;
    std::optional<int> reflow_position;
    GtkTextTag *paragraph, *code, *html, *rule, *table_row, *table_head, *table_last, *quote;
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
    struct DiagramAnchor { GtkWidget* box; GtkTextChildAnchor* anchor; unsigned depth; };
    std::vector<std::optional<MermaidResult>> diagram_results;
    std::vector<std::vector<DiagramAnchor>> diagram_widgets;
    guint diagram_resize = 0;
    int diagram_view_width = 0;
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
        auto* preview_text = static_cast<PreviewTextView*>(g_object_new(preview_text_view_get_type(),
                                                                       "buffer", buffer, nullptr));
        preview_text->owner = this;
        preview_text->draw_tables = +[](void* owner, cairo_t* cr) { static_cast<Impl*>(owner)->draw_tables(cr); };
        view = GTK_WIDGET(preview_text);
        gtk_widget_set_name(view, "markdown-text");
        g_signal_connect(view, "size-allocate", G_CALLBACK(+[](GtkWidget*, GtkAllocation*, Impl* self) {
            self->schedule_table_reflow();
            self->schedule_diagram_resize();
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
        gtk_style_context_add_provider(gtk_widget_get_style_context(view), GTK_STYLE_PROVIDER(css),
                                       GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
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
        quote = gtk_text_buffer_create_tag(buffer, "md-quote", nullptr);
        code = gtk_text_buffer_create_tag(buffer, "md-code-block", "family", "monospace",
            "scale", .93, nullptr);
        html = gtk_text_buffer_create_tag(buffer, "md-html", "family", "monospace", nullptr);
        rule = gtk_text_buffer_create_tag(buffer, "md-rule", nullptr);
        table_row = gtk_text_buffer_create_tag(buffer, "md-cell", "font", table_font,
            "wrap-mode", GTK_WRAP_NONE, "direction", GTK_TEXT_DIR_LTR,
            "pixels-above-lines", row_padding, "pixels-below-lines", row_padding, "pixels-inside-wrap", 1, nullptr);
        table_head = gtk_text_buffer_create_tag(buffer, "md-table-head", "weight", PANGO_WEIGHT_BOLD,
            "pixels-above-lines", row_padding + table_spacing, nullptr);
        table_last = gtk_text_buffer_create_tag(buffer, nullptr, "pixels-below-lines", row_padding + table_spacing, nullptr);
        emphasis = gtk_text_buffer_create_tag(buffer, "md-em", "style", PANGO_STYLE_ITALIC, nullptr);
        strong = gtk_text_buffer_create_tag(buffer, "md-strong", "weight", PANGO_WEIGHT_BOLD, nullptr);
        inline_code = gtk_text_buffer_create_tag(buffer, "md-code", "family", "monospace", nullptr);
        strike = gtk_text_buffer_create_tag(buffer, "md-strike", "strikethrough", TRUE, nullptr);
        alt = gtk_text_buffer_create_tag(buffer, "md-alt", "style", PANGO_STYLE_ITALIC, nullptr);
        link = gtk_text_buffer_create_tag(buffer, "md-link", "underline", PANGO_UNDERLINE_SINGLE, nullptr);
        apply_colors();
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
        g_object_unref(css);
    }

    void apply_colors() {
        const auto style = "#markdown-text, #markdown-text text { background-color: " + palette.background +
            "; color: " + palette.text + "; font: 12pt sans-serif; }"
            "#markdown-text selection { background-color: " + palette.selection +
            "; color: " + palette.selection_text + "; }";
        gtk_css_provider_load_from_data(css, style.c_str(), style.size(), nullptr);
        g_object_set(quote, "foreground", palette.muted.c_str(), "paragraph-background", palette.surface.c_str(), nullptr);
        g_object_set(code, "foreground", palette.code_text.c_str(), "paragraph-background", palette.surface.c_str(), nullptr);
        g_object_set(html, "foreground", palette.code_text.c_str(), nullptr);
        g_object_set(rule, "foreground", palette.border.c_str(), nullptr);
        g_object_set(table_row, "foreground", palette.text.c_str(), nullptr);
        g_object_set(table_head, "foreground", palette.text.c_str(), nullptr);
        g_object_set(inline_code, "foreground", palette.code_text.c_str(), "background", palette.raised.c_str(), nullptr);
        g_object_set(link, "foreground", palette.accent.c_str(), nullptr);
        gtk_widget_queue_draw(view);
    }

    GtkTextBuffer* buffer() const { return gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)); }

    void cancel_reflow() {
        if (reflow_timer) { g_source_remove(reflow_timer); reflow_timer = 0; }
        reflow_position.reset();
    }

    int available_width() const {
        const int allocated = gtk_widget_get_allocated_width(view);
        return allocated > 1 ? std::max(180, allocated - gtk_text_view_get_left_margin(GTK_TEXT_VIEW(view)) -
            gtk_text_view_get_right_margin(GTK_TEXT_VIEW(view))) : 760;
    }

    void schedule_table_reflow() {
        if (!page || !page->table_fit_width || !g_object_get_data(G_OBJECT(box), "markdown-ready")) return;
        if (reflow_timer) { g_source_remove(reflow_timer); reflow_timer = 0; }
        const int natural = page->table_fit_width;
        if (std::abs(std::min(available_width(), natural) - std::min(rendered_width, natural)) < 8) return;
        // Let a window or sidebar drag settle before reshaping table cells.
        reflow_timer = g_timeout_add(100, +[](gpointer data) -> gboolean {
            auto* self = static_cast<Impl*>(data);
            self->reflow_timer = 0;
            GdkRectangle visible;
            auto* text = GTK_TEXT_VIEW(self->view);
            gtk_text_view_get_visible_rect(text, &visible);
            GtkTextIter top;
            gtk_text_view_get_line_at_y(text, &top, visible.y, nullptr);
            const auto row = gtk_text_iter_get_line(&top);
            self->start(self->page->index, 0, false);
            self->reflow_position = row;
            return G_SOURCE_REMOVE;
        }, this);
    }

    void stop() {
        cancel_reflow();
        if (diagram_resize) { g_source_remove(diagram_resize); diagram_resize = 0; }
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
        rendered_width = available_width();
        worker->submit(Request{document->snapshot(), index, offset, locate, stride, [token = receiver, revision] {
            auto* self = *token;
            if (!self || self->diagram_generation != revision) return;
            auto result = self->worker->take();
            if (!result) return;
            self->page = std::move(result);
            self->row_tabs.resize(self->page->table_columns.size());
            self->diagram_widgets.resize(self->page->diagrams.size());
            self->diagram_results.resize(self->page->diagrams.size());
            self->refresh_diagrams();
            if (!self->batch()) { self->finish(); return; }
            self->source = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer data) -> gboolean {
                auto* self = static_cast<Impl*>(data);
                if (self->batch()) return G_SOURCE_CONTINUE;
                self->source = 0;
                self->finish();
                return G_SOURCE_REMOVE;
            }, self, nullptr);
        }, rendered_width, dpi});
    }

    void refresh_diagrams() {
        if (!page || page->diagrams.empty()) return;
        const auto revision = ++diagram_generation;
        worker->submit(DiagramRequest{page->diagrams, palette, [token = receiver, revision](unsigned index, MermaidResult result) {
            auto* self = *token;
            if (!self || self->diagram_generation != revision) return;
            auto& previous = self->diagram_results[index];
            if (previous && previous->status == result.status && previous->image == result.image) return;
            previous = std::move(result);
            for (const auto& widget : self->diagram_widgets[index]) self->present_diagram(index, widget);
        }});
    }

    int diagram_width(const DiagramAnchor& anchor, GdkPixbuf* original) const {
        auto* text = GTK_TEXT_VIEW(view);
        const auto width = gtk_widget_get_allocated_width(view);
        const int margins = gtk_text_view_get_left_margin(text) + gtk_text_view_get_right_margin(text);
        const int available = width > 1 ? width - margins - 18 * int(anchor.depth) : 720;
        return std::min(gdk_pixbuf_get_width(original), std::max(1, available));
    }

    void fit_diagram(GtkImage* image, GdkPixbuf* original, int width) {
        if (width == gdk_pixbuf_get_width(original)) {
            gtk_image_set_from_pixbuf(image, original);
            return;
        }
        int height = std::max(1, (gdk_pixbuf_get_height(original) * width + gdk_pixbuf_get_width(original) / 2) /
                                 gdk_pixbuf_get_width(original));
        auto* scaled = gdk_pixbuf_scale_simple(original, width, height, GDK_INTERP_BILINEAR);
        gtk_image_set_from_pixbuf(image, scaled);
        g_object_unref(scaled);
    }

    void relayout_diagram(const DiagramAnchor& anchor) {
        // GTK 3 can keep the placeholder's old Pango shape after an anchored
        // child changes requisition. Reattaching invalidates this line only.
        if (!gtk_widget_get_visible(anchor.box)) return;
        g_object_ref(anchor.box);
        gtk_container_remove(GTK_CONTAINER(view), anchor.box);
        gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(view), anchor.box, anchor.anchor);
        g_object_unref(anchor.box);
    }

    void schedule_diagram_resize() {
        int width = gtk_widget_get_allocated_width(view);
        if (width == diagram_view_width) return;
        diagram_view_width = width;
        if (diagram_widgets.empty() || diagram_resize) return;
        diagram_resize = g_idle_add_full(G_PRIORITY_HIGH_IDLE, +[](gpointer data) -> gboolean {
            auto* self = static_cast<Impl*>(data);
            self->diagram_resize = 0;
            for (unsigned i = 0; i < self->diagram_widgets.size(); ++i) {
                const auto& result = self->diagram_results[i];
                if (!result || !result->image) continue;
                for (const auto& anchor : self->diagram_widgets[i]) {
                    auto* image = GTK_IMAGE(gtk_bin_get_child(GTK_BIN(anchor.box)));
                    const auto target = self->diagram_width(anchor, result->image.get());
                    if (gdk_pixbuf_get_width(gtk_image_get_pixbuf(image)) == target) continue;
                    self->fit_diagram(image, result->image.get(), target);
                    self->relayout_diagram(anchor);
                }
            }
            return G_SOURCE_REMOVE;
        }, this, nullptr);
    }

    void present_diagram(unsigned index, const DiagramAnchor& anchor) {
        auto* child = gtk_bin_get_child(GTK_BIN(anchor.box));
        const auto& result = diagram_results[index];
        if (GTK_IS_IMAGE(child) && result && result->image) {
            fit_diagram(GTK_IMAGE(child), result->image.get(), diagram_width(anchor, result->image.get()));
            relayout_diagram(anchor);
            return;
        }
        if (child) gtk_widget_destroy(child);
        if (!result) {
            child = gtk_label_new("Rendering Mermaid diagram…");
        } else if (result->image) {
            child = gtk_image_new();
            fit_diagram(GTK_IMAGE(child), result->image.get(), diagram_width(anchor, result->image.get()));
            gtk_widget_set_name(child, "markdown-mermaid-image");
            atk_object_set_name(gtk_widget_get_accessible(child), "Mermaid diagram");
        } else {
            child = gtk_expander_new(result->status == MermaidStatus::Invalid
                ? "Could not render Mermaid diagram" : "Mermaid renderer unavailable");
            auto* label = gtk_label_new(page->diagrams[index].c_str());
            gtk_label_set_selectable(GTK_LABEL(label), TRUE);
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
            gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
            gtk_label_set_max_width_chars(GTK_LABEL(label), 65);
            gtk_container_add(GTK_CONTAINER(child), label);
            gtk_widget_show(label);
        }
        gtk_container_add(GTK_CONTAINER(anchor.box), child);
        gtk_widget_show(child);
        relayout_diagram(anchor);
    }

    void draw_tables(cairo_t* cr) const {
        if (!page || page->table_rows.empty()) return;
        auto* text = GTK_TEXT_VIEW(view);
        double x1, y1, x2, y2;
        cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
        GtkTextIter top, bottom;
        gtk_text_view_get_line_at_y(text, &top, static_cast<int>(y1), nullptr);
        gtk_text_view_get_line_at_y(text, &bottom, static_cast<int>(y2), nullptr);
        const auto& rows = page->table_rows;
        auto row = std::lower_bound(rows.begin(), rows.end(), gtk_text_iter_get_offset(&top),
            [](const TableRow& candidate, int pos) { return candidate.end <= pos; });
        const int visible_end = gtk_text_iter_get_offset(&bottom) + gtk_text_iter_get_chars_in_line(&bottom);
        GdkRGBA header, stripe, border;
        gdk_rgba_parse(&header, palette.raised.c_str());
        gdk_rgba_parse(&stripe, palette.surface.c_str());
        gdk_rgba_parse(&border, palette.border.c_str());
        for (; row != rows.end() && row->begin < visible_end; ++row) {
            GtkTextIter begin;
            gtk_text_buffer_get_iter_at_offset(buffer(), &begin, row->begin);
            int y = 0, height = 0;
            gtk_text_view_get_line_yrange(text, &begin, &y, &height);
            if (y >= y2) break;
            if (row->header) { y += table_spacing; height -= table_spacing; }
            if (row->last) height -= table_spacing;
            const auto& columns = page->table_columns[row->columns];
            GdkRectangle first;
            gtk_text_view_get_iter_location(text, &begin, &first);
            const int left = first.x;
            const int width = columns.back().right;
            if (row->header || row->stripe) {
                gdk_cairo_set_source_rgba(cr, row->header ? &header : &stripe);
                cairo_rectangle(cr, left, y, width, height);
                cairo_fill(cr);
            }
            // Use filled one-pixel rules, including the outside edges, so
            // adjacent cells share a single border at every scale factor.
            gdk_cairo_set_source_rgba(cr, &border);
            if (row->header) cairo_rectangle(cr, left, y, width, 1);
            cairo_rectangle(cr, left, y + height - 1, width, 1);
            cairo_rectangle(cr, left, y, 1, height);
            for (const auto& col : columns)
                cairo_rectangle(cr, left + col.right - 1, y, 1, height);
            cairo_fill(cr);
        }
    }

    GtkTextTag* table_tab_tag(unsigned index) {
        auto*& tag = row_tabs[index];
        if (tag) return tag;
        const auto& stops = page->table_columns[index];
        auto* tabs = pango_tab_array_new(static_cast<int>(stops.size()) + 1, TRUE);
        for (unsigned i = 0; i < stops.size(); ++i) {
            auto align = PANGO_TAB_LEFT;
            if (stops[i].align == MarkdownAlign::Center) align = PANGO_TAB_CENTER;
            if (stops[i].align == MarkdownAlign::Right) align = PANGO_TAB_RIGHT;
            pango_tab_array_set_tab(tabs, i, align, stops[i].stop);
        }
        pango_tab_array_set_tab(tabs, stops.size(), PANGO_TAB_LEFT, stops.back().right);
        tag = gtk_text_buffer_create_tag(buffer(), nullptr, "tabs", tabs, nullptr);
        pango_tab_array_free(tabs);
        return tag;
    }

    void apply_block(Format f, GtkTextIter& begin, GtkTextIter& end) {
        auto add = [&](GtkTextTag* tag) { gtk_text_buffer_apply_tag(buffer(), tag, &begin, &end); };
        switch (f.kind) {
        case MarkdownKind::Heading: add(headings[std::clamp<unsigned>(f.level, 1, 6)]); break;
        case MarkdownKind::Code: add(code); break;
        case MarkdownKind::Html: add(html); break;
        case MarkdownKind::Rule: add(rule); break;
        case MarkdownKind::TableHeader: add(table_row); add(table_head); break;
        case MarkdownKind::TableRow: add(table_row); break;
        default: add(paragraph); break;
        }
        if (f.depth) add(indents[f.depth]);
        if (f.quoted && f.tabs == none) add(quote);
        if (f.tabs != none) add(table_tab_tag(f.tabs));
        if (f.last_table_row) add(table_last);
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
                    auto& slot = diagram_widgets[first_run.diagram].emplace_back(widget, anchor, first_run.format.depth);
                    present_diagram(first_run.diagram, slot);
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
            GtkTextIter top;
            gtk_text_buffer_get_iter_at_line(buffer(), &top,
                std::min(*reflow_position, gtk_text_buffer_get_line_count(buffer()) - 1));
            scroll(gtk_text_iter_get_offset(&top));
            reflow_position.reset();
        } else scroll(page->target);
        std::vector<Run>().swap(page->runs);
        std::vector<Pixbuf>().swap(page->images);
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
void MarkdownPreview::set_palette(const Palette& palette) {
    if (impl_->destroyed || impl_->palette == palette) return;
    impl_->palette = palette;
    impl_->apply_colors();
    impl_->refresh_diagrams();
}
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
