#include "virtual_terminal.hpp"

#include "unicode_width_oracle.hpp"

#include <algorithm>
#include <charconv>
#include <optional>

namespace my_agent::test {

namespace {

// 解码一个 UTF-8 序列，返回码点与消耗字节数。非法序列返回 {U+FFFD, 1}。
// 这是探针侧的独立实现 —— 与生产渲染路径同源会让两边一起错。
struct Decoded {
    char32_t code_point;
    std::size_t size;
};

// SGR 的参数表。空参数串按 ECMA-48 等价于单个 0（重置）。返回 nullopt
// 表示参数不是纯数字分段（例如冒号分隔的 4:3）—— 那就是没建模的形态。
[[nodiscard]]
std::optional<std::vector<int>> sgr_parameters(std::string_view params)
{
    std::vector<int> list;
    if (params.empty()) {
        list.push_back(0);
        return list;
    }
    std::size_t at = 0;
    while (true) {
        const std::size_t separator = params.find(';', at);
        const std::string_view piece = params.substr(
            at, separator == std::string_view::npos ? std::string_view::npos
                                                    : separator - at
        );
        int value = 0;
        if (!piece.empty()) {
            if (std::from_chars(piece.data(), piece.data() + piece.size(), value).ec
                != std::errc{}) {
                return std::nullopt;
            }
        }
        list.push_back(value);
        if (separator == std::string_view::npos) {
            return list;
        }
        at = separator + 1;
    }
}

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
    row[column].style = current_style_;
    if (width == 2 && column + 1 < static_cast<std::size_t>(columns_)) {
        row[column + 1].text.clear();
        row[column + 1].continuation = true;
        // 续格与首格是同一笔画出来的：样式必须同源，否则「宽字符宽度」
        // 这类按格比较的断言会在两格上读到不同的样式。
        row[column + 1].style = current_style_;
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
    // 擦除后填充的是**当前背景色**（BCE），不是完整继承当前 SGR：前景、粗体、
    // 反显等都不属于擦除出的空白格。探针若把完整 current_style_ 搬过去，会让
    // 一个从未被画过的空格伪装成 styled caret。
    const Cell erased{.style = CellStyle{
        .bg = current_style_.bg,
        .bg_rgb = current_style_.bg_rgb,
    }};
    auto& row = grid_[static_cast<std::size_t>(cursor_row_)];
    for (auto column = static_cast<std::size_t>(cursor_column_); column < row.size();
         ++column) {
        row[column] = erased;
    }
}

void VirtualTerminal::erase_to_end_of_screen()
{
    erase_to_end_of_line();
    const Cell erased{.style = CellStyle{
        .bg = current_style_.bg,
        .bg_rgb = current_style_.bg_rgb,
    }};
    for (auto row = static_cast<std::size_t>(cursor_row_) + 1; row < grid_.size(); ++row) {
        std::ranges::fill(grid_[row], erased);
    }
}

bool VirtualTerminal::apply_sgr(std::string_view params)
{
    const std::optional<std::vector<int>> parsed = sgr_parameters(params);
    if (!parsed) {
        return false;
    }

    // 先在新状态上走完整条序列，成功才提交 —— 半途失败的序列不该留下
    // 一半的样式（探针要么完整建模，要么整条交给 unhandled）。
    CellStyle next = current_style_;
    const auto set_color = [&next](bool foreground, int value) {
        if (foreground) {
            next.fg = value;
            next.fg_rgb = 0;
        } else {
            next.bg = value;
            next.bg_rgb = 0;
        }
    };

    const std::vector<int>& list = *parsed;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const int value = list[i];
        switch (value) {
            case 0:
                next = CellStyle{};
                break;
            case 1:
                next.bold = true;
                break;
            case 2:
                next.dim = true;
                break;
            case 3:
                next.italic = true;
                break;
            case 4:
                next.underline = true;
                break;
            case 7:
                next.inverse = true;
                break;
            case 9:
                next.strikethrough = true;
                break;
            case 22:
                next.bold = false;
                next.dim = false;
                break;
            case 23:
                next.italic = false;
                break;
            case 24:
                next.underline = false;
                break;
            case 27:
                next.inverse = false;
                break;
            case 29:
                next.strikethrough = false;
                break;
            case 39:
                set_color(true, CellStyle::kDefault);
                break;
            case 49:
                set_color(false, CellStyle::kDefault);
                break;
            case 38:
            case 48: {
                // 扩展色。两种标准形态之外（含子参数不够、分量越界）算没建模。
                const bool foreground = (value == 38);
                if (i + 1 >= list.size()) {
                    return false;
                }
                const int mode = list[i + 1];
                if (mode == 5) {
                    if (i + 2 >= list.size() || list[i + 2] < 0 || list[i + 2] > 255) {
                        return false;
                    }
                    set_color(foreground, 16 + list[i + 2]);
                    i += 2;
                } else if (mode == 2) {
                    if (i + 4 >= list.size()) {
                        return false;
                    }
                    unsigned rgb = 0;
                    for (int component = 1; component <= 3; ++component) {
                        const int channel = list[i + 1 + static_cast<std::size_t>(component)];
                        if (channel < 0 || channel > 255) {
                            return false;
                        }
                        rgb = (rgb << 8) | static_cast<unsigned>(channel);
                    }
                    if (foreground) {
                        next.fg = CellStyle::kTrueColor;
                        next.fg_rgb = rgb;
                    } else {
                        next.bg = CellStyle::kTrueColor;
                        next.bg_rgb = rgb;
                    }
                    i += 4;
                } else {
                    return false;
                }
                break;
            }
            default:
                if (value >= 30 && value <= 37) {
                    set_color(true, value - 30);
                } else if (value >= 90 && value <= 97) {
                    set_color(true, value - 90 + 8);
                } else if (value >= 40 && value <= 47) {
                    set_color(false, value - 40);
                } else if (value >= 100 && value <= 107) {
                    set_color(false, value - 100 + 8);
                } else {
                    return false;  // 未建模：闪烁 / 隐藏 / 上划线 … 一律落 unhandled
                }
                break;
        }
    }
    current_style_ = next;
    return true;
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
        case 'm':  // SGR：改写当前绘图状态，后续绘制的格子带上它
            if (!apply_sgr(params)) {
                unhandled_.emplace_back(
                    std::string{"CSI "} + std::string{params} + final_byte
                );
            }
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
    const Cell& cell = line.at(static_cast<std::size_t>(column));
    // 宽字符的续格是「被占用的空白」：它没有自己的文本，但那两列属于
    // 同一个字形，不该被当成空格子。
    return cell.text.empty() && !cell.continuation;
}

std::string VirtualTerminal::cell_text(int row, int column) const
{
    const std::vector<Cell>& line = grid_.at(static_cast<std::size_t>(row));
    return line.at(static_cast<std::size_t>(column)).text;
}

CellStyle VirtualTerminal::cell_style(int row, int column) const
{
    const std::vector<Cell>& line = grid_.at(static_cast<std::size_t>(row));
    return line.at(static_cast<std::size_t>(column)).style;
}

}  // namespace my_agent::test
