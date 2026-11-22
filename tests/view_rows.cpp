#include "view_rows.hpp"

#include <iostream>
#include <random>
#include <stdexcept>

using slate::view::Pos;
using slate::view::Rows;

void check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

void verify_rows(const std::string& text) {
    slate::Document doc;
    doc.replace(0, 0, text);
    Rows rows(doc);
    Pos pos = 0;
    for (;;) {
        auto row = rows.at(pos);
        check(row.start == pos && row.end <= row.next && row.next <= doc.size(), "invalid extent");
        check(row.next - pos <= 2056, "unbounded viewport row");
        check(rows.start(pos) == pos, "invalid forward boundary");
        if (pos != row.next)
            check(rows.start(row.next - 1) == pos, "inconsistent previous boundary");
        for (Pos inside = pos + 1; inside < row.next; inside += 197)
            check(rows.start(inside) == pos, "inconsistent random-access boundary");
        if (row.newline) {
            check(text.at(row.next - 1) == '\n', "missing line terminator");
            check(row.end == pos || text.at(row.end - 1) != '\r' || text.at(row.end) == '\r',
                  "unexpected CR in display text");
        }
        if (rows.last(row)) {
            check(rows.start(doc.size()) == pos, "invalid terminal boundary");
            break;
        }
        check(row.next > pos, "no forward progress");
        pos = row.next;
    }
}

int main() {
    try {
        verify_rows("");
        verify_rows("\r\n\n");
        verify_rows(std::string(100000, 'a'));
        for (std::size_t offset = 1017; offset < 1031; ++offset) {
            auto prefix = std::string(offset, 'x');
            auto suffix = std::string(20000, 'y');
            verify_rows(prefix + "\r\n" + suffix);
            verify_rows(prefix + "🍎🍐🍊🍋" + suffix);
            verify_rows(prefix + "\n" + suffix);
        }
        std::mt19937 rng(1618);
        std::string content;
        for (int i = 0; i < 150; ++i) {
            content.append(rng() % 12500, 'a');
            content += rng() % 2 ? "\r\n" : "\n";
            content += "夏🍏";
        }
        verify_rows(content);
        slate::Document doc;
        doc.replace(0, 0, "é\r\n🪶X");
        Rows rows(doc);
        for (auto [from, to] : {std::pair<Pos, Pos>{0, 2}, {2, 4}, {4, 8}, {8, 9}})
            check(rows.right(from) == to && rows.left(to) == from, "UTF-8 or CRLF navigation failed");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
