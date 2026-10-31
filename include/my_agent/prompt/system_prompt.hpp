#pragma once

#include <string>
#include <vector>

namespace my_agent::prompt {

// 构建系统提示所需的外部事实。宿主在 effect 侧采集它们（读环境、读文件），
// build() 本身保持纯 —— 这样提示的内容可以不碰文件系统直接断言。
struct Context {
    std::string working_directory;
    std::string operating_system;

    // M7 的注入点。memory 条目与 skill 目录都要读文件，所以由调用方填。
    std::vector<std::string> memories;
    std::string skills_catalog;
};

// 按 agentty 的风格分节：每个可变部分是一个 XML 标签块，模型容易定位，我们也
// 容易在不动静态基底的前提下增删某一节。
[[nodiscard]]
std::string build(const Context& context);

// 宿主操作系统。**进程级**事实：会话之间不会不同，所以留在 prompt，
// 不进会话服务。判据就这一句 —— 会话之间会不会不同。
//
// working_directory 曾经也从这里采集（那时函数叫 capture_environment），
// TS-003 把它还给了会话：它**会**随会话不同（cwd 是会话构造时定下的边界）。
// 一个仍叫 capture_environment、却把 working_directory 留空的函数是误导，
// 所以那个名字一并删掉。
[[nodiscard]]
std::string host_operating_system();

}  // namespace my_agent::prompt
