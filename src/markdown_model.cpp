#include "markdown_model.hpp"

// MD4C v0.6.0, https://github.com/mity/md4c/tree/7fc1815a5eeba2af7d6120a76202bf59f3b6e6e4
#include "md4c.h"
extern "C" {
#include "entity.h"
}
#include <glib.h>

#include <algorithm>
#include <charconv>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace slate {
namespace {

std::uint32_t offset(std::size_t n) {
    if (n > UINT32_MAX) throw std::length_error("Markdown preview exceeds the parser's size limit");
    return static_cast<std::uint32_t>(n);
}

void append_utf8(std::string& out, std::string_view text) {
    if (text.empty()) return;
    if (g_utf8_validate(text.data(), text.size(), nullptr)) {
        out.append(text);
        return;
    }
    std::unique_ptr<gchar, decltype(&g_free)> repaired(g_utf8_make_valid(text.data(), text.size()), &g_free);
    out.append(repaired.get());
}

void append_scalar(std::string& out, unsigned value) {
    char bytes[6];
    if (value == 0 || !g_unichar_validate(value)) value = 0xfffd;
    out.append(bytes, g_unichar_to_utf8(value, bytes));
}

void append_text(std::string& out, MD_TEXTTYPE type, std::string_view text) {
    switch (type) {
    case MD_TEXT_NULLCHAR: out.append("�"); return;
    case MD_TEXT_BR: out.push_back('\n'); return;
    case MD_TEXT_SOFTBR: out.push_back(' '); return;
    case MD_TEXT_ENTITY:
        // MD4C has already validated the syntax, including the final semicolon.
        if (text[1] == '#') {
            const bool hex = text[2] == 'x' || text[2] == 'X';
            unsigned value = 0;
            std::from_chars(text.data() + (hex ? 3 : 2), text.data() + text.size() - 1,
                            value, hex ? 16 : 10);
            append_scalar(out, value);
        } else if (const auto* entity = entity_lookup(text.data(), text.size())) {
            append_scalar(out, entity->codepoints[0]);
            if (entity->codepoints[1]) append_scalar(out, entity->codepoints[1]);
        } else {
            out.append(text);
        }
        return;
    default: append_utf8(out, text); return;
    }
}

std::string attribute(const MD_ATTRIBUTE& attr) {
    std::string result;
    for (unsigned i = 0, pos = 0; pos < attr.size; ++i) {
        auto next = attr.substr_offsets[i + 1];
        append_text(result, attr.substr_types[i], {attr.text + pos, next - pos});
        pos = next;
    }
    return result;
}

struct List {
    char delimiter = 0; // Zero for unordered lists.
    unsigned next = 0;
};

struct Inline {
    unsigned style = 0;
    std::uint32_t destination = no_destination;
    std::uint32_t enclosing_link = no_destination;
    std::uint32_t image_begin = 0;
};

class Builder {
public:
    Builder(std::string_view input, const std::function<bool()>& cancelled, const MarkdownLimits* limits)
        : input_(input), cancelled_(cancelled), limits_(limits) {}

    std::optional<MarkdownModel> parse() {
        MD_PARSER parser{};
        parser.flags = MD_FLAG_TABLES | MD_FLAG_TASKLISTS | MD_FLAG_STRIKETHROUGH |
                       MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_COLLAPSEWHITESPACE;
        parser.enter_block = [](MD_BLOCKTYPE type, void* detail, void* self) {
            return static_cast<Builder*>(self)->dispatch(&Builder::enter_block, type, detail);
        };
        parser.leave_block = [](MD_BLOCKTYPE type, void*, void* self) {
            return static_cast<Builder*>(self)->dispatch(&Builder::leave_block, type);
        };
        parser.enter_span = [](MD_SPANTYPE type, void* detail, void* self) {
            return static_cast<Builder*>(self)->dispatch(&Builder::enter_span, type, detail);
        };
        parser.leave_span = [](MD_SPANTYPE type, void*, void* self) {
            return static_cast<Builder*>(self)->dispatch(&Builder::leave_span, type);
        };
        parser.text = [](MD_TEXTTYPE type, const char* bytes, MD_SIZE size, void* self) {
            return static_cast<Builder*>(self)->dispatch(&Builder::text, type, std::string_view(bytes, size));
        };

        if (cancelled_ && cancelled_()) return std::nullopt;
        if (input_.size() > std::numeric_limits<int>::max())
            throw std::length_error("Markdown input exceeds the parser's size limit");
        const int rc = md_parse(input_.empty() ? "" : input_.data(), offset(input_.size()), &parser, this);
        if (error_) std::rethrow_exception(error_);
        if (rc < 0) throw std::runtime_error("Could not parse Markdown");
        if (rc || stop_requested()) return std::nullopt;
        return std::move(model_);
    }

private:
    bool stop_requested() const {
        return (cancelled_ && cancelled_()) ||
               (limits_ && (model_.blocks.size() > limits_->blocks || model_.spans.size() > limits_->spans ||
                            model_.cells.size() > limits_->cells));
    }

    // Return control to MD4C on error so that it can release its C allocations.
    template<class Method, class... Args>
    int dispatch(Method method, Args... args) noexcept {
        try {
            if ((++callback_count_ & 63) == 0 && stop_requested()) return 1;
            (this->*method)(args...);
            return 0;
        } catch (...) {
            error_ = std::current_exception();
            return 1;
        }
    }

    void begin_block(MarkdownKind kind) {
        end_block();
        auto& block = model_.blocks.emplace_back();
        block.kind = kind;
        block.begin = offset(model_.text.size());
        block.span_begin = offset(model_.spans.size());
        block.cell_begin = offset(model_.cells.size());
        block.source_offset = last_line_;
        block.list_depth = offset(lists_.size());
        block.quote_depth = quote_depth_;
        block.marker_begin = offset(model_.markers.size());
        model_.markers.append(marker_);
        marker_.clear();
        block.marker_end = offset(model_.markers.size());
        open_ = true;
        needs_source_ = true;
    }

    void end_block() {
        if (!open_) return;
        auto& block = model_.blocks.back();
        // Verbatim callbacks terminate even the final line. Block separation is
        // supplied by the renderer, so drop that one terminal newline.
        if ((block.kind == MarkdownKind::Code || block.kind == MarkdownKind::Html ||
             block.kind == MarkdownKind::Mermaid) &&
            model_.text.size() > block.begin && model_.text.back() == '\n') model_.text.pop_back();
        block.end = offset(model_.text.size());
        block.span_end = offset(model_.spans.size());
        block.cell_end = offset(model_.cells.size());
        open_ = false;
    }

    void finish_item() {
        if (!marker_.empty()) begin_block(MarkdownKind::Paragraph);
        end_block();
    }

    void enter_block(MD_BLOCKTYPE type, void* detail) {
        switch (type) {
        case MD_BLOCK_QUOTE: end_block(); ++quote_depth_; break;
        case MD_BLOCK_UL: finish_item(); lists_.push_back({}); break;
        case MD_BLOCK_OL: {
            finish_item();
            const auto& list = *static_cast<MD_BLOCK_OL_DETAIL*>(detail);
            lists_.push_back({list.mark_delimiter, list.start});
            break;
        }
        case MD_BLOCK_LI: {
            const auto& item = *static_cast<MD_BLOCK_LI_DETAIL*>(detail);
            auto& list = lists_.back();
            if (item.is_task) marker_ = item.task_mark == ' ' ? "☐" : "☑";
            else if (list.delimiter) marker_ = std::to_string(list.next) + list.delimiter;
            else marker_ = "•";
            ++list.next;
            break;
        }
        case MD_BLOCK_P: begin_block(MarkdownKind::Paragraph); break;
        case MD_BLOCK_H:
            begin_block(MarkdownKind::Heading);
            model_.blocks.back().level = static_cast<MD_BLOCK_H_DETAIL*>(detail)->level;
            break;
        case MD_BLOCK_CODE: {
            const auto& code = *static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
            const bool mermaid = code.fence_char && code.lang.size == 7 &&
                                 g_ascii_strncasecmp(code.lang.text, "mermaid", 7) == 0;
            begin_block(mermaid ? MarkdownKind::Mermaid : MarkdownKind::Code);
            break;
        }
        case MD_BLOCK_HR: begin_block(MarkdownKind::Rule); break;
        case MD_BLOCK_HTML: begin_block(MarkdownKind::Html); break;
        case MD_BLOCK_THEAD: table_header_ = true; break;
        case MD_BLOCK_TR: begin_block(table_header_ ? MarkdownKind::TableHeader : MarkdownKind::TableRow); break;
        case MD_BLOCK_TD:
        case MD_BLOCK_TH: {
            if (model_.blocks.back().cell_begin != model_.cells.size()) model_.text.push_back('\t');
            const auto align = static_cast<MD_BLOCK_TD_DETAIL*>(detail)->align;
            const auto pos = offset(model_.text.size());
            static constexpr MarkdownAlign aligns[] = {MarkdownAlign::Default, MarkdownAlign::Left,
                                                       MarkdownAlign::Center, MarkdownAlign::Right};
            model_.cells.push_back({pos, pos, aligns[align]});
            break;
        }
        default: break;
        }
    }

    void leave_block(MD_BLOCKTYPE type) {
        switch (type) {
        case MD_BLOCK_QUOTE: end_block(); --quote_depth_; break;
        case MD_BLOCK_LI: finish_item(); break;
        case MD_BLOCK_UL: case MD_BLOCK_OL: lists_.pop_back(); break;
        case MD_BLOCK_THEAD: table_header_ = false; break;
        case MD_BLOCK_TH: case MD_BLOCK_TD: model_.cells.back().end = offset(model_.text.size()); break;
        case MD_BLOCK_DOC: case MD_BLOCK_P: case MD_BLOCK_H: case MD_BLOCK_HR:
        case MD_BLOCK_CODE: case MD_BLOCK_HTML: case MD_BLOCK_TR: end_block(); break;
        default: break;
        }
    }

    std::uint32_t destination(const MD_ATTRIBUTE& attr) {
        const auto index = offset(model_.destinations.size());
        model_.destinations.push_back(attribute(attr));
        return index;
    }

    void enter_span(MD_SPANTYPE type, void* detail) {
        // MD4C omits paragraph callbacks for tight list items.
        if (!open_) begin_block(MarkdownKind::Paragraph);
        styles_.push_back(style_);
        if (style_.style & MdImage) return; // The outer image uses all nested text as its alt.
        switch (type) {
        case MD_SPAN_EM: style_.style |= MdEmphasis; break;
        case MD_SPAN_STRONG: style_.style |= MdStrong; break;
        case MD_SPAN_DEL: style_.style |= MdStrike; break;
        case MD_SPAN_CODE: style_.style |= MdCode; break;
        case MD_SPAN_A:
            style_.style |= MdLink;
            style_.destination = destination(static_cast<MD_SPAN_A_DETAIL*>(detail)->href);
            break;
        case MD_SPAN_IMG:
            style_.style |= MdImage;
            style_.enclosing_link = style_.destination;
            style_.destination = destination(static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src);
            style_.image_begin = offset(model_.text.size());
            break;
        default: break;
        }
    }

    void leave_span(MD_SPANTYPE type) {
        if (type == MD_SPAN_IMG && !(styles_.back().style & MdImage) && style_.image_begin == model_.text.size()) {
            model_.text.append("￼"); // U+FFFC object replacement character for empty alt text.
            style_run(style_.image_begin);
        }
        style_ = styles_.back();
        styles_.pop_back();
    }

    void style_run(std::uint32_t begin) {
        if (!style_.style || model_.text.size() == begin) return;
        const auto end = offset(model_.text.size());
        if (model_.spans.size() > model_.blocks.back().span_begin) {
            auto& prev = model_.spans.back();
            if (prev.end == begin && prev.style == style_.style && prev.destination == style_.destination &&
                prev.enclosing_link == style_.enclosing_link) {
                prev.end = end;
                return;
            }
        }
        model_.spans.push_back({begin, end, style_.style, style_.destination, style_.enclosing_link});
    }

    void locate_source(std::string_view text) {
        // MD4C sometimes passes temporary buffers or static punctuation.
        auto ptr = reinterpret_cast<std::uintptr_t>(text.data());
        auto base = reinterpret_cast<std::uintptr_t>(input_.data());
        if (ptr < base || ptr - base >= input_.size()) return;
        auto pos = offset(ptr - base);
        while (pos > last_line_ && input_[pos - 1] != '\n' && input_[pos - 1] != '\r') --pos;
        last_line_ = std::max(last_line_, pos);
        model_.blocks.back().source_offset = last_line_;
        needs_source_ = false;
    }

    void text(MD_TEXTTYPE type, std::string_view value) {
        if (!open_) begin_block(MarkdownKind::Paragraph);
        if (needs_source_) locate_source(value);
        const auto begin = offset(model_.text.size());
        append_text(model_.text, type, value);
        style_run(begin);
    }

    std::string_view input_;
    const std::function<bool()>& cancelled_;
    const MarkdownLimits* limits_;
    MarkdownModel model_;
    std::exception_ptr error_;
    std::vector<List> lists_;
    std::vector<Inline> styles_;
    Inline style_;
    std::string marker_;
    unsigned quote_depth_ = 0, callback_count_ = 0;
    std::uint32_t last_line_ = 0;
    bool open_ = false, table_header_ = false, needs_source_ = false;
};

} // namespace

std::optional<MarkdownModel> parse_markdown(std::string_view input, const std::function<bool()>& cancelled,
                                           const MarkdownLimits* limits) {
    return Builder(input, cancelled, limits).parse();
}

} // namespace slate
