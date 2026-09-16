#include "virtual_terminal.hpp"

#include "unicode_width_oracle.hpp"

#include <algorithm>
#include <charconv>

namespace my_agent::test {

namespace {

// 解码一个 UTF-8 序列，返回码点与消耗字节数。非法序列返回 {U+FFFD, 1}。
// 这是探针侧的独立实现 —— 与生产渲染路径同源会让两边一起错。
struct Decoded {
    char32_t code_point;
    std::size_t size;
};

[[nodiscard]]
Decoded decode(std::string_view text) noexcept
{
    const auto lead = static_cast<unsigned char>(text[0]);
    std::size_t length = 1;
    char32_t code_point = lead;
    if (lead >= 0xF0) {
        length = 4;
        code_point = lead & 0x07u;
    } else if (lead >= 0xE0) {
        length = 3;
        code_point = lead & 0x0Fu;
    } else if (lead >= 0xC0) {
        length = 2;
        code_point = lead & 0x1Fu;
    } else if (lead >= 0x80) {
        return {0xFFFD, 1};
    }
    if (text.size() < length) {
        return {0xFFFD, 1};
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
        code_point = (code_point << 6)
                     | (static_cast<unsigned char>(text[offset]) & 0x3Fu);
    }
    return {code_point, length};
}

}  // namespace

VirtualTerminal::VirtualTerminal(int columns, int rows)
    : columns_{columns},
      rows_{rows},
      grid_(static_cast<std::size_t>(rows),
            std::vector<Cell>(static_cast<std::size_t>(columns)))
{
}

void VirtualTerminal::scroll_up()
{
    grid_.erase(grid_.begin());
    grid_.emplace_back(static_cast<std::size_t>(columns_));
    if (alt_screen_) {
        ++alt_scrolls_;
    }
}

void VirtualTerminal::put(char32_t code_point, int width)
{
    // DECAWM 开时：光标停在末列后是「待换行」状态，**下一个**可打印字符才真正换行
    // （ECMA-48 §8.3.118 / DEC STD 070）。这个延迟是关键 —— 正好填满一行不会立刻
    // 滚动，所以「填满」这件事本身无害，害在填满之后还有字符要写。
    if (pending_wrap_) {
        ++overruns_;
        pending_wrap_ = false;
        cursor_column_ = 0;
        if (cursor_row_ + 1 >= rows_) {
            scroll_up();
        } else {
            ++cursor_row_;
        }
    }

    // 宽字符放不进剩余空间：同样要换行，否则会被切成两半跨行。
    if (width == 2 && cursor_column_ + 2 > columns_) {
        ++overruns_;
        cursor_column_ = 0;
        if (cursor_row_ + 1 >= rows_) {
            scroll_up();
        } else {
            ++cursor_row_;
        }
    }

    auto& row = grid_[static_cast<std::size_t>(cursor_row_)];
    const auto column = static_cast<std::size_t>(cursor_column_);
    row[column].text.clear();
    // 把码点写回 UTF-8：屏幕内容要能和期望字符串直接比。
    if (code_point < 0x80) {
        row[column].text.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        row[column].text.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        row[column].text.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        row[column].text.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        row[column].text.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        row[column].text.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        row[column].text.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        row[column].text.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        row[column].text.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        row[column].text.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
    row[column].continuation = false;
    if (width == 2 && column + 1 < static_cast<std::size_t>(columns_)) {
        row[column + 1].text.clear();
        row[column + 1].continuation = true;
    }

    cursor_column_ += width;
    if (cursor_column_ >= columns_) {
        if (autowrap_) {
            // 待换行：不立刻动，等下一个字符。
            cursor_column_ = columns_ - 1;
            pending_wrap_ = true;
        } else {
            // DECAWM 关：光标钉在末列，后续字符原地覆写，永不滚动。
            cursor_column_ = columns_ - 1;
        }
    }
}

void VirtualTerminal::erase_to_end_of_line()
{
    auto& row = grid_[static_cast<std::size_t>(cursor_row_)];
    for (auto column = static_cast<std::size_t>(cursor_column_); column < row.size();
         ++column) {
        row[column] = Cell{};
    }
}

void VirtualTerminal::erase_to_end_of_screen()
{
    erase_to_end_of_line();
    for (auto row = static_cast<std::size_t>(cursor_row_) + 1; row < grid_.size(); ++row) {
        std::ranges::fill(grid_[row], Cell{});
    }
}

void VirtualTerminal::apply_csi(std::string_view params, char final_byte)
{
    const auto number = [params](int fallback) {
        int value = fallback;
        const auto* const begin = params.data();
        const auto* const end = begin + params.size();
        if (std::from_chars(begin, end, value).ec != std::errc{}) {
            return fallback;
        }
        return value;
    };

    // 私有模式：?7 是 DECAWM，?1049 是备用屏，?25 是光标可见性。
    if (!params.empty() && params.front() == '?'
        && (final_byte == 'h' || final_byte == 'l')) {
        const bool set = final_byte == 'h';
        const std::string_view mode = params.substr(1);
        if (mode == "7") {
            autowrap_ = set;
            pending_wrap_ = false;  // 切换 DECAWM 清掉待换行状态
            return;
        }
        if (mode == "1049") {
            alt_screen_ = set;
            for (auto& row : grid_) {
                std::ranges::fill(row, Cell{});
            }
            cursor_row_ = 0;
            cursor_column_ = 0;
            pending_wrap_ = false;
            return;
        }
        if (mode == "2026") {
            return;  // synchronized output; no effect on cell accounting
        }
        if (mode == "25") {
            return;  // 光标可见性不影响记账
        }
    }

    switch (final_byte) {
        case 'H': {  // CUP：行;列，1-based。任何定位都清掉待换行状态。
            int row = 1;
            int column = 1;
            const auto semicolon = params.find(';');
            if (semicolon == std::string_view::npos) {
                row = number(1);
            } else {
                const std::string_view row_text = params.substr(0, semicolon);
                const std::string_view column_text = params.substr(semicolon + 1);
                std::from_chars(row_text.data(), row_text.data() + row_text.size(), row);
                std::from_chars(
                    column_text.data(), column_text.data() + column_text.size(), column
                );
            }
            cursor_row_ = std::clamp(row - 1, 0, rows_ - 1);
            cursor_column_ = std::clamp(column - 1, 0, columns_ - 1);
            pending_wrap_ = false;
            return;
        }
        case 'K':  // EL：0/缺省擦到行尾
            if (number(0) == 0) {
                erase_to_end_of_line();
                return;
            }
            break;
        case 'J':  // ED：0/缺省擦到屏幕底
            if (number(0) == 0) {
                erase_to_end_of_screen();
                return;
            }
            break;
        case 'm':  // SGR：颜色/样式不影响格子记账
            return;
        default:
            break;
    }
    unhandled_.emplace_back(std::string{"CSI "} + std::string{params} + final_byte);
}

void VirtualTerminal::feed(std::string_view bytes)
{
    while (!bytes.empty()) {
        if (bytes.front() == '\x1b') {
            if (bytes.size() >= 2 && bytes[1] == '[') {
                // CSI：参数字节 0x30..0x3F，中间字节 0x20..0x2F，终止字节 0x40..0x7E。
                std::size_t index = 2;
                while (index < bytes.size()
                       && (static_cast<unsigned char>(bytes[index]) < 0x40
                           || static_cast<unsigned char>(bytes[index]) > 0x7E)) {
                    ++index;
                }
                if (index >= bytes.size()) {
                    return;  // 序列被切断在缓冲末尾，等下一次 feed
                }
                apply_csi(bytes.substr(2, index - 2), bytes[index]);
                bytes.remove_prefix(index + 1);
                continue;
            }
            // 非 CSI 的转义序列：本项目不发，记下来让探针能断言它没出现。
            unhandled_.emplace_back("ESC " + std::string{bytes.substr(1, 1)});
            bytes.remove_prefix(std::min<std::size_t>(2, bytes.size()));
            continue;
        }

        if (bytes.front() == '\r') {
            cursor_column_ = 0;
            pending_wrap_ = false;
            bytes.remove_prefix(1);
            continue;
        }
        if (bytes.front() == '\n') {
            pending_wrap_ = false;
            if (cursor_row_ + 1 >= rows_) {
                scroll_up();
            } else {
                ++cursor_row_;
            }
            bytes.remove_prefix(1);
            continue;
        }

        const Decoded decoded = decode(bytes);
        const int width = oracle_char_width(decoded.code_point);
        if (width == kUnknownWidth) {
            // 判据不认识这个码点。**不能猜 1** —— 猜小会把真实的溢出算成合规，
            // 探针于是在最需要它的时候失去鉴别力。记下来让断言直接失败。
            unhandled_.emplace_back(
                "unknown code point U+"
                + [](char32_t value) {
                      std::string hex;
                      for (int shift = 20; shift >= 0; shift -= 4) {
                          const int digit = static_cast<int>((value >> shift) & 0xF);
                          if (!hex.empty() || digit != 0 || shift == 0) {
                              hex.push_back(static_cast<char>(
                                  digit < 10 ? '0' + digit : 'A' + digit - 10
                              ));
                          }
                      }
                      return hex;
                  }(decoded.code_point)
            );
            bytes.remove_prefix(decoded.size);
            continue;
        }
        put(decoded.code_point, width);
        bytes.remove_prefix(decoded.size);
    }
}

std::vector<std::string> VirtualTerminal::screen() const
{
    std::vector<std::string> rows;
    rows.reserve(grid_.size());
    for (const std::vector<Cell>& row : grid_) {
        std::string text;
        for (const Cell& cell : row) {
            if (cell.continuation) {
                continue;  // 宽字符的第二格不产出字节
            }
            text += cell.text.empty() ? " " : cell.text;
        }
        while (!text.empty() && text.back() == ' ') {
            text.pop_back();
        }
        rows.push_back(std::move(text));
    }
    return rows;
}

bool VirtualTerminal::cell_blank(int row, int column) const
{
    const std::vector<Cell>& line = grid_.at(static_cast<std::size_t>(row));
    return line.at(static_cast<std::size_t>(column)).text.empty();
}

}  // namespace my_agent::test
