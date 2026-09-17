#pragma once

// 测试专用的最小虚拟终端。存在的理由：pty 本身**不解释**转义序列 —— 它只是一对
// fd。所以「屏幕上有没有幽灵行」这个问题，光看抓到的字节流是回答不了的，必须把
// 字节流喂进一个会记账的模型里。
//
// 刻意只实现本项目真正发出的那几个序列：CUP、EL(0)、ED(0)、SGR、DECAWM 的 ?7h/?7l、
// 备用屏 ?1049h/l、同步输出 ?2026h/l、光标 ?25h/l。遇到没实现的序列会记进
// unhandled() —— 静默忽略会让探针在实现变化后悄悄失去鉴别力，而那正是「断言通过但
// 没有鉴别力」最隐蔽的一种。
//
// 宽度用 unicode_width_oracle.hpp（官方 Unicode 数据），**不用**生产渲染路径的宽度
// 判据：用被测的宽度函数驱动虚拟终端，欠算会在模型和实现里同时发生、互相抵消，
// 溢出因此永远量不出来。
//
// 用 cell 网格而不是按行累加字符串：宽字符占两格、覆写要按列生效，这两件事
// 字符串模型都表达不了。刻意不针对任何一个渲染后端的当前字节形状做简化 —— 那样
// 一旦实现改动，探针会悄悄不再鉴别。

#include <cstddef>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace my_agent::test {

// 一格上生效的 SGR 事实 —— 「这一格被画成了什么样」。存在的理由：有一类被画
// 出来的格子在纯文本网格里**根本看不见** —— 光标格就是典型：它靠反显/前景色
// 把自己与周围的正文区分开，正文内容却可能一模一样（maya 的 composer 甚至
// 发同一串块字形字节，只换样式来表示「看得见 / 看不见」）。只记账文本的探针
// 对这类格子没有鉴别力。
//
// 刻意只做结构化记录与相等比较，不做语义解释：什么颜色算「高对比」、哪个
// 属性算「暗掉」是主题与断言的事，不是探针的事。未建模的 SGR 参数照旧落进
// unhandled()（与转义序列同一条纪律：静默忽略会让探针在实现变化后悄悄失去
// 鉴别力）。
struct CellStyle {
    // fg/bg 的取值域：
    //   kDefault   终端默认
    //   0..7       基本色（SGR 30-37 / 40-47）
    //   8..15      亮色（SGR 90-97 / 100-107 归一化到 +8）
    //   16..271    256 色（SGR 38;5;N ⇒ 16+N），与色号同构可比
    //   kTrueColor 24 位真彩（SGR 38;2;r;g;b），分量在 fg_rgb / bg_rgb
    static constexpr int kDefault = -1;
    static constexpr int kTrueColor = -2;

    int fg{kDefault};
    int bg{kDefault};
    unsigned fg_rgb{};
    unsigned bg_rgb{};
    bool bold{};
    bool dim{};
    bool italic{};
    bool underline{};
    bool strikethrough{};
    bool inverse{};

    [[nodiscard]] bool operator==(const CellStyle&) const = default;
};

// gtest 的失败输出。默认打印会把这种结构体渲染成一串裸字节
// （"24-byte object <0F-00 00-00 …>"），断言样式时根本读不出差异。
inline void PrintTo(const CellStyle& style, std::ostream* out)
{
    *out << "style{fg=" << style.fg << ", bg=" << style.bg;
    if (style.fg == CellStyle::kTrueColor || style.bg == CellStyle::kTrueColor) {
        *out << ", rgb=" << style.fg_rgb << "/" << style.bg_rgb;
    }
    if (style.bold) {
        *out << ", bold";
    }
    if (style.dim) {
        *out << ", dim";
    }
    if (style.italic) {
        *out << ", italic";
    }
    if (style.underline) {
        *out << ", underline";
    }
    if (style.strikethrough) {
        *out << ", strikethrough";
    }
    if (style.inverse) {
        *out << ", inverse";
    }
    *out << "}";
}

class VirtualTerminal {
public:
    VirtualTerminal(int columns, int rows);

    void feed(std::string_view bytes);

    // 备用屏里发生过的滚动次数。**这是幽灵行的直接判据**：备用屏是定高的，
    // 帧字节全部用绝对定位（CUP）画，一旦滚动，此后每一次 CUP 都落在错位的
    // 物理行上 —— 屏幕上表现为整块内容上移、顶部那行被顶掉，而新画的内容与
    // 旧内容错开，也就是「重复打印 / 幽灵行」看起来的样子。
    [[nodiscard]] int scrolls_in_alt_screen() const noexcept { return alt_scrolls_; }

    // 当前屏幕内容，每行一个字符串（行尾空白已去掉）。
    [[nodiscard]] std::vector<std::string> screen() const;

    // 指定格是否空白（宽字符的续格也算占用格）。按列断言 gutter / 边缘
    // 空白用：screen() 已把行尾空白裁掉，字节串答不了「第 N 列是什么」。
    [[nodiscard]] bool cell_blank(int row, int column) const;

    // 指定格的文本（空白格返回空串；宽字符的续格也返回空串 —— 它的内容
    // 在首格上）。screen() 的字符串下标在含宽字符的行上不等于显示列，
    // 所以按列定位内容只能用这个。
    [[nodiscard]] std::string cell_text(int row, int column) const;

    // 指定格被画出来时生效的样式。空白格返回它被擦除/跳过时的样式。
    [[nodiscard]] CellStyle cell_style(int row, int column) const;

    // 遇到过的未实现序列，原样记下以便探针断言它是空的。
    [[nodiscard]] const std::vector<std::string>& unhandled() const noexcept
    {
        return unhandled_;
    }

    [[nodiscard]] bool autowrap() const noexcept { return autowrap_; }
    [[nodiscard]] bool in_alt_screen() const noexcept { return alt_screen_; }

    // 光标曾经越过右边距的次数（DECAWM 开时会触发换行，关时会停在末列）。
    [[nodiscard]] int right_margin_overruns() const noexcept { return overruns_; }

private:
    struct Cell {
        std::string text;      // 空表示空白
        bool continuation{};   // 宽字符的第二格
        CellStyle style;       // 画这一格时生效的样式
    };

    void put(char32_t code_point, int width);
    void scroll_up();
    void apply_csi(std::string_view params, char final_byte);
    // SGR 参数 → current_style_ 的迁移。返回 false 表示有参数没建模，
    // 调用方据此落进 unhandled()。整条序列要么全生效要么不动。
    [[nodiscard]] bool apply_sgr(std::string_view params);
    void erase_to_end_of_line();
    void erase_to_end_of_screen();

    int columns_;
    int rows_;
    std::vector<std::vector<Cell>> grid_;
    std::vector<std::string> unhandled_;
    int cursor_row_ = 0;     // 0-based
    int cursor_column_ = 0;  // 0-based
    bool autowrap_ = true;   // DECAWM 开机默认开，这正是幽灵行的成因 1
    bool pending_wrap_ = false;
    bool alt_screen_ = false;
    int alt_scrolls_ = 0;
    int overruns_ = 0;
    CellStyle current_style_;  // SGR 建立的当前绘图状态
};

}  // namespace my_agent::test
