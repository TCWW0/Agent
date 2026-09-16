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
#include <string>
#include <string_view>
#include <vector>

namespace my_agent::test {

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
    };

    void put(char32_t code_point, int width);
    void scroll_up();
    void apply_csi(std::string_view params, char final_byte);
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
};

}  // namespace my_agent::test
