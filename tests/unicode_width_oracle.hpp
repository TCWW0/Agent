#pragma once

// 测试专用的独立宽度判据。**不复用生产渲染路径的宽度判据** —— 用它量宽度的测试
// 会按构造必然通过，永远无法与实现分歧。
//
// 真相来源是 Unicode 16.0.0 官方 `EastAsianWidth.txt`：属性 W 或 F 即 2 列。
// 这里只硬编码测试向量里真实用到的那些区间，逐条注明官方数据的出处。刻意不做成
// 完整表 —— 完整表会重新引入「和实现同源」的风险，而窄表加 unknown 兜底能保证
// 遇到没预期的字符时**大声失败**而不是悄悄给个错数。
//
// 未知码点返回 kUnknownWidth（-1），调用方必须显式处理。返回 1 兜底会让探针在
// 遇到意外字符时给出偏小的宽度，从而把真实的溢出算成合规。

#include <cstddef>
#include <string_view>

namespace my_agent::test {

inline constexpr int kUnknownWidth = -1;

// 单个码点的显示宽度，按官方 East_Asian_Width。
[[nodiscard]]
inline int oracle_char_width(char32_t code_point) noexcept
{
    // 本项目帧字节里出现的窄字符：ASCII 可打印区 + 空格。
    if (code_point >= 0x20 && code_point < 0x7F) {
        return 1;
    }
    // Dock chrome 字形（issue #26 起 maya widget 输出）：
    //   Box Drawing（U+2500..257F：─│╭╮╰╯┊…）、Block Elements
    //   （U+2580..259F：█▌…，自绘光标的字形区）、·（U+00B7 hint 分隔）、
    //   ↵⇧⌥（U+21B5/21E7/2325，hint row 键位字形）、❯（U+276F prompt）、
    //   …（U+2026 省略号，placeholder 字形）、●/◆/⚠（phase glyph）。
    // EastAsianWidth-16.0.0 里均为 N 或 A（个别），宽 1 —— 与生产侧
    // 「不在宽表即 1 列」一致。收录理由同既有条目：帧字节真实出现。
    if ((code_point >= 0x2500 && code_point <= 0x257F)
        || (code_point >= 0x2580 && code_point <= 0x259F)
        || code_point == 0x00B7 || code_point == 0x2026
        || code_point == 0x25CF || code_point == 0x25C6
        || code_point == 0x26A0
        || code_point == 0x21B5
        || code_point == 0x21E7 || code_point == 0x2325
        || code_point == 0x276F) {
        return 1;
    }
    struct Wide {
        char32_t first;
        char32_t last;
    };
    // 全部来自 EastAsianWidth-16.0.0.txt 中属性为 W 的行：
    constexpr Wide kWide[] = {
        {0x26A1, 0x26A1},      // `26A1 ; W # So HIGH VOLTAGE SIGN`
        {0x2705, 0x2705},      // `2705 ; W # So WHITE HEAVY CHECK MARK`
        {0x274C, 0x274C},      // `274C ; W # So CROSS MARK`
        {0x2B50, 0x2B50},      // `2B50 ; W # So WHITE MEDIUM STAR`
        {0x4E00, 0x9FFF},      // `4E00..9FFF ; W # Lo CJK UNIFIED IDEOGRAPH`
        {0x1F7E0, 0x1F7EB},    // `1F7E0..1F7EB ; W # So [12] LARGE ORANGE CIRCLE..`
        {0x20000, 0x2A6DF},    // `20000..2A6DF ; W # Lo [42720] CJK EXT B`
        {0x2CEB0, 0x2EBE0},    // `2CEB0..2EBE0 ; W # Lo [7473] CJK EXT F`
    };
    for (const Wide& range : kWide) {
        if (code_point >= range.first && code_point <= range.last) {
            return 2;
        }
    }
    return kUnknownWidth;
}

// UTF-8 文本的显示宽度。任何一个码点未知就整体返回 kUnknownWidth ——
// 「大部分字符认得」不足以支撑一条关于溢出的断言。
[[nodiscard]]
inline int oracle_display_width(std::string_view text) noexcept
{
    int width = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
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
            return kUnknownWidth;  // 落单的续字节：不猜
        }
        if (index + length > text.size()) {
            return kUnknownWidth;  // 截断的序列：不猜
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            code_point = (code_point << 6)
                         | (static_cast<unsigned char>(text[index + offset]) & 0x3Fu);
        }
        const int single = oracle_char_width(code_point);
        if (single == kUnknownWidth) {
            return kUnknownWidth;
        }
        width += single;
        index += length;
    }
    return width;
}

}  // namespace my_agent::test
