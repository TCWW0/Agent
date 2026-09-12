cmake_minimum_required(VERSION 4.4)
project (my_agent
    VERSION 0.0.1
    LANGUAGES CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723
)

set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)

# cpp-httplib 的 CMake 在找到 OpenSSL 时会以 INTERFACE 方式传播
# CPPHTTPLIB_OPENSSL_SUPPORT，我们不手动定义该宏。REQUIRE_OPENSSL 让缺少
# OpenSSL 时直接失败，而不是静默退化成没有 HTTPS 的构建。
set(HTTPLIB_REQUIRE_OPENSSL ON CACHE INTERNAL "")
set(HTTPLIB_TEST OFF CACHE INTERNAL "")
set(HTTPLIB_INSTALL OFF CACHE INTERNAL "")
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.52.0
    GIT_SHALLOW TRUE
)

if(MSVC)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
endif()

FetchContent_MakeAvailable(googletest nlohmann_json httplib)

# ── maya: TUI rendering library (slice #15) ─────────────────────────────────
# We take maya ONLY for its renderer. The single seam we use is
#   FrameBuffer::render(const Element&, const Theme&) -> const std::string&
# plus commit(). We deliberately DO NOT use maya's run<Program>() event loop:
# its Program concept demands update(Model, Msg) -> pair<Model, Cmd<Msg>>,
# which would take over this project's core AsyncHost/WorkPool/WakeSignal loop
# — the very thing this project exists to demonstrate. Rendering only; the
# event loop stays ours. Do not reintroduce run<Program>() as a "simplification".
#
# Pinned to the same commit as agentty's maya submodule (2026-07-21). Fetched
# from the upstream git repo (not the local ../agentty path) so we get a clean
# pin unaffected by any local submodule drift.
#
# Two options MUST be forced off before MakeAvailable:
#   MAYA_BUILD_EXAMPLES — we don't build maya's demos.
#   MAYA_NATIVE_TUNING  — default ON bakes -march=native into the binary,
#                         which would make our build non-portable.
# (maya's tests/fuzzers already default OFF.)
set(MAYA_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(MAYA_NATIVE_TUNING  OFF CACHE INTERNAL "")
FetchContent_Declare(
    maya
    GIT_REPOSITORY https://github.com/1ay1/maya.git
    GIT_TAG cc35bcf5faf8dfddb284b0ac5a531025873765da
    SYSTEM
)
FetchContent_MakeAvailable(maya)

include(GoogleTest)

enable_testing()

find_package(Threads REQUIRED)

add_library(
    my_agent_tools
    src/tool/read.cpp
    src/tool/registry.cpp
    src/tool/memory_store.cpp
    src/tool/memory_tools.cpp
    src/tool/skills.cpp
    src/tool/skill_tools.cpp
)

target_include_directories(
    my_agent_tools
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_tools
    PUBLIC cxx_std_26
)

target_link_libraries(
    my_agent_tools
    PUBLIC nlohmann_json::nlohmann_json
)

# 传输层：httplib 只在 .cpp 内可见，头文件不泄漏 HTTP 库细节
add_library(
    my_agent_http
    src/http/http_client.cpp
)

target_include_directories(
    my_agent_http
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_http
    PUBLIC cxx_std_26
)

target_link_libraries(
    my_agent_http
    PUBLIC Threads::Threads
    PRIVATE httplib::httplib
)

add_executable(
    http_smoke_test
    tests/http_smoke_test.cpp
)

target_link_libraries(
    http_smoke_test
    PRIVATE
        my_agent_http
        GTest::gtest_main
)

gtest_discover_tests(
    http_smoke_test
    TEST_PREFIX "http_smoke."
)

add_library(
    my_agent_core
    src/domain/conversation.cpp
    src/runtime/agent.cpp
)

target_include_directories(
    my_agent_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_core
    PUBLIC cxx_std_26
)

target_link_libraries(
    my_agent_core
    PUBLIC my_agent_tools
)

add_executable(
    agent_loop_test
    tests/agent_loop_test.cpp
)

target_link_libraries(
    agent_loop_test
    PRIVATE
        my_agent_core
        GTest::gtest_main
)

gtest_discover_tests(
    agent_loop_test
    TEST_PREFIX "agent_loop."
)

add_library(
    my_agent_headless
    src/runtime/headless_runner.cpp
)

target_include_directories(
    my_agent_headless
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_headless
    PUBLIC cxx_std_26
)

target_link_libraries(
    my_agent_headless
    PUBLIC my_agent_core
)

add_executable(
    headless_runner_test
    tests/headless_runner_test.cpp
)

target_link_libraries(
    headless_runner_test
    PRIVATE
        my_agent_headless
        GTest::gtest_main
)

gtest_discover_tests(
    headless_runner_test
    TEST_PREFIX "headless_runner."
)

add_library(
    my_agent_async_runtime
    src/runtime/async_host.cpp
    src/runtime/wake_signal.cpp
    src/runtime/work_pool.cpp
)

target_include_directories(
    my_agent_async_runtime
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_async_runtime
    PUBLIC cxx_std_26
)

target_link_libraries(
    my_agent_async_runtime
    PUBLIC
        my_agent_core
        Threads::Threads
)

add_executable(
    async_host_test
    tests/async_host_test.cpp
)

target_link_libraries(
    async_host_test
    PRIVATE
        my_agent_async_runtime
        GTest::gtest_main
)

gtest_discover_tests(
    async_host_test
    TEST_PREFIX "async_host."
)

add_executable(
    tool_loop_test
    tests/tool_loop_test.cpp
)

target_link_libraries(
    tool_loop_test
    PRIVATE
        my_agent_headless
        GTest::gtest_main
)

gtest_discover_tests(
    tool_loop_test
    TEST_PREFIX "tool_loop."
)

add_executable(
    tool_policy_test
    tests/tool_policy_test.cpp
)

target_link_libraries(
    tool_policy_test
    PRIVATE
        my_agent_tools
        GTest::gtest_main
)

gtest_discover_tests(
    tool_policy_test
    TEST_PREFIX "tool_policy."
)

add_executable(
    profile_test
    tests/profile_test.cpp
)

target_link_libraries(
    profile_test
    PRIVATE
        my_agent_core
        GTest::gtest_main
)

gtest_discover_tests(
    profile_test
    TEST_PREFIX "profile."
)

add_executable(
    memory_store_test
    tests/memory_store_test.cpp
)
target_link_libraries(
    memory_store_test
    PRIVATE
        my_agent_tools
        GTest::gtest_main
)
gtest_discover_tests(
    memory_store_test
    TEST_PREFIX "memory_store."
)

add_executable(
    skills_test
    tests/skills_test.cpp
)
target_link_libraries(
    skills_test
    PRIVATE
        my_agent_tools
        GTest::gtest_main
)
gtest_discover_tests(
    skills_test
    TEST_PREFIX "skills."
)

add_executable(
    read_tool_test
    tests/read_tool_test.cpp
)
target_link_libraries(
    read_tool_test
    PRIVATE
        my_agent_tools
        GTest::gtest_main
)
gtest_discover_tests(
    read_tool_test
    TEST_PREFIX "read_tool."
)

add_executable(
    wake_signal_test
    tests/wake_signal_test.cpp
)

target_link_libraries(
    wake_signal_test
    PRIVATE
        my_agent_async_runtime
        GTest::gtest_main
)

gtest_discover_tests(
    wake_signal_test
    TEST_PREFIX "wake_signal."
)

add_executable(
    work_pool_test
    tests/work_pool_test.cpp
)

target_link_libraries(
    work_pool_test
    PRIVATE
        my_agent_async_runtime
        GTest::gtest_main
)

gtest_discover_tests(
    work_pool_test
    TEST_PREFIX "work_pool."
)

# Provider 传输层：把线上 wire 格式翻译成 Msg
add_library(
    my_agent_provider
    src/provider/ollama.cpp
    src/provider/sse.cpp
    src/provider/anthropic.cpp
    src/provider/openai.cpp
)

target_include_directories(
    my_agent_provider
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_provider
    PUBLIC cxx_std_26
)

target_link_libraries(
    my_agent_provider
    PUBLIC
        my_agent_core
        my_agent_http
)

add_executable(
    ollama_transport_test
    tests/ollama_transport_test.cpp
)

target_link_libraries(
    ollama_transport_test
    PRIVATE
        my_agent_provider
        GTest::gtest_main
)

gtest_discover_tests(
    ollama_transport_test
    TEST_PREFIX "ollama_transport."
)

add_executable(
    sse_transport_test
    tests/sse_transport_test.cpp
)

target_link_libraries(
    sse_transport_test
    PRIVATE
        my_agent_provider
        GTest::gtest_main
)

gtest_discover_tests(
    sse_transport_test
    TEST_PREFIX "sse_transport."
)

add_executable(
    anthropic_transport_test
    tests/anthropic_transport_test.cpp
)

target_link_libraries(
    anthropic_transport_test
    PRIVATE
        my_agent_provider
        GTest::gtest_main
)

gtest_discover_tests(
    anthropic_transport_test
    TEST_PREFIX "anthropic_transport."
)

add_executable(
    openai_transport_test
    tests/openai_transport_test.cpp
)

target_link_libraries(
    openai_transport_test
    PRIVATE
        my_agent_provider
        GTest::gtest_main
)

gtest_discover_tests(
    openai_transport_test
    TEST_PREFIX "openai_transport."
)

# 系统提示构建器：build() 保持纯，只有 capture_environment 碰 IO
add_library(
    my_agent_prompt
    src/prompt/system_prompt.cpp
)

target_include_directories(
    my_agent_prompt
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_prompt
    PUBLIC cxx_std_26
)

add_executable(
    system_prompt_test
    tests/system_prompt_test.cpp
)

target_link_libraries(
    system_prompt_test
    PRIVATE
        my_agent_prompt
        GTest::gtest_main
)

gtest_discover_tests(
    system_prompt_test
    TEST_PREFIX "system_prompt."
)

# 测试专用量具：最小虚拟终端。pty 不解释转义序列，所以「屏幕上有没有幽灵行」
# 必须把字节流喂进一个会记账的模型才能回答。不进产品库 —— 它只服务测试。
add_library(
    my_agent_virtual_terminal
    tests/virtual_terminal.cpp
)

target_include_directories(
    my_agent_virtual_terminal
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/tests"
)

target_compile_features(
    my_agent_virtual_terminal
    PUBLIC cxx_std_26
)

add_executable(
    virtual_terminal_test
    tests/virtual_terminal_test.cpp
)

target_link_libraries(
    virtual_terminal_test
    PRIVATE
        my_agent_virtual_terminal
        GTest::gtest_main
)

gtest_discover_tests(
    virtual_terminal_test
    TEST_PREFIX "virtual_terminal."
)

# 幽灵行 pty 复现探针。真 pty + 虚拟终端 + 官方宽度判据三层叠起来，才能把
# 「屏幕上有幽灵行」变成一条会失败的断言。切片 #12 刻意让它红着。
add_executable(
    ghost_line_probe_test
    tests/ghost_line_probe_test.cpp
)

target_link_libraries(
    ghost_line_probe_test
    PRIVATE
        my_agent_ui_loop
        my_agent_virtual_terminal
        GTest::gtest_main
)

gtest_discover_tests(
    ghost_line_probe_test
    TEST_PREFIX "ghost_line_probe."
)

# TUI 视图投影：Model -> Frame。纯函数，不碰终端，可脱离 tty 单测。
add_library(
    my_agent_ui_markdown
    src/ui/markdown_scanner.cpp
)

target_include_directories(
    my_agent_ui_markdown
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_ui_markdown
    PUBLIC cxx_std_26
)

add_library(
    my_agent_ui_view
    src/ui/view.cpp
)

target_include_directories(
    my_agent_ui_view
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_ui_view
    PUBLIC cxx_std_26
)

target_link_libraries(
    my_agent_ui_view
    PUBLIC
        my_agent_core
        my_agent_ui_input_layout
        my_agent_ui_markdown
        my_agent_ui_status_bar
)

add_library(
    my_agent_ui_status_bar
    src/ui/status_bar.cpp
)

target_include_directories(
    my_agent_ui_status_bar
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_ui_status_bar
    PUBLIC cxx_std_26
)

add_executable(
    status_bar_test
    tests/status_bar_test.cpp
)

target_link_libraries(
    status_bar_test
    PRIVATE
        my_agent_ui_status_bar
        GTest::gtest_main
)

gtest_discover_tests(
    status_bar_test
    TEST_PREFIX "status_bar."
)

add_executable(
    view_test
    tests/view_test.cpp
)

target_link_libraries(
    view_test
    PRIVATE
        my_agent_ui_view
        GTest::gtest_main
)

gtest_discover_tests(
    view_test
    TEST_PREFIX "view."
)

add_executable(
    markdown_scanner_test
    tests/markdown_scanner_test.cpp
)

target_include_directories(
    markdown_scanner_test
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    markdown_scanner_test
    PRIVATE cxx_std_26
)

target_link_libraries(
    markdown_scanner_test
    PRIVATE
        my_agent_ui_markdown
        GTest::gtest_main
)

gtest_discover_tests(
    markdown_scanner_test
    TEST_PREFIX "markdown_scanner."
)

# TUI Maya projection: our own Frame/StyledLine IR -> maya::Element. This is the
# only place the pure view projection learns about Maya types.
add_library(
    my_agent_ui_maya
    src/ui/maya_projection.cpp
)

target_include_directories(
    my_agent_ui_maya
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_ui_maya
    PUBLIC cxx_std_26
)

target_link_libraries(
    my_agent_ui_maya
    PUBLIC
        my_agent_ui_view
        maya
)

add_executable(
    maya_projection_test
    tests/maya_projection_test.cpp
)

target_link_libraries(
    maya_projection_test
    PRIVATE
        my_agent_ui_maya
        my_agent_virtual_terminal
        GTest::gtest_main
)

gtest_discover_tests(
    maya_projection_test
    TEST_PREFIX "maya_projection."
)

# TUI 事件循环：apply_key 是纯的（按键 -> UiState 变更 + 可选 Msg），
# run_ui 才碰 poll 与终端。分开是为了让按键语义脱离 tty 可测。
add_library(
    my_agent_ui_loop
    src/ui/resize_watch.cpp
    src/ui/ui_loop.cpp
)

target_include_directories(
    my_agent_ui_loop
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_ui_loop
    PUBLIC cxx_std_26
)

target_link_libraries(
    my_agent_ui_loop
    PUBLIC
        my_agent_async_runtime
        my_agent_ui_clock
        my_agent_ui_input_editor
        my_agent_ui_input
        my_agent_ui_terminal
)

add_executable(
    resize_watch_test
    tests/resize_watch_test.cpp
)

target_link_libraries(
    resize_watch_test
    PRIVATE
        my_agent_ui_loop
        GTest::gtest_main
)

gtest_discover_tests(
    resize_watch_test
    TEST_PREFIX "resize_watch."
)

add_executable(
    ui_loop_test
    tests/ui_loop_test.cpp
)

target_link_libraries(
    ui_loop_test
    PRIVATE
        my_agent_ui_loop
        GTest::gtest_main
)

gtest_discover_tests(
    ui_loop_test
    TEST_PREFIX "ui_loop."
)

# TUI 按键解码：字节流 -> 按键。纯状态机，不碰终端，跨 read 边界的 UTF-8 在这里缝合。
add_library(
    my_agent_ui_input
    src/ui/input.cpp
)

target_include_directories(
    my_agent_ui_input
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_ui_input
    PUBLIC cxx_std_26
)

add_executable(
    input_test
    tests/input_test.cpp
)

target_link_libraries(
    input_test
    PRIVATE
        my_agent_ui_input
        GTest::gtest_main
)

gtest_discover_tests(
    input_test
    TEST_PREFIX "input."
)

add_library(
    my_agent_ui_input_editor
    src/ui/input_editor.cpp
)

target_include_directories(
    my_agent_ui_input_editor
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_ui_input_editor
    PUBLIC cxx_std_26
)

target_link_libraries(
    my_agent_ui_input_editor
    PUBLIC
        my_agent_ui_input
)

add_executable(
    input_editor_test
    tests/input_editor_test.cpp
)

target_link_libraries(
    input_editor_test
    PRIVATE
        my_agent_ui_input_editor
        GTest::gtest_main
)

gtest_discover_tests(
    input_editor_test
    TEST_PREFIX "input_editor."
)

add_library(
    my_agent_ui_input_layout
    src/ui/input_layout.cpp
)

target_include_directories(
    my_agent_ui_input_layout
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_ui_input_layout
    PUBLIC cxx_std_26
)

add_executable(
    input_layout_test
    tests/input_layout_test.cpp
)

target_link_libraries(
    input_layout_test
    PRIVATE
        my_agent_ui_input_layout
        GTest::gtest_main
)

gtest_discover_tests(
    input_layout_test
    TEST_PREFIX "input_layout."
)

# TUI 帧率合并：纯判定，时间由调用方注入，因此可确定性测试而无需 sleep。
add_library(
    my_agent_ui_clock
    src/ui/repaint_clock.cpp
)

target_include_directories(
    my_agent_ui_clock
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_ui_clock
    PUBLIC cxx_std_26
)

add_executable(
    repaint_clock_test
    tests/repaint_clock_test.cpp
)

target_link_libraries(
    repaint_clock_test
    PRIVATE
        my_agent_ui_clock
        GTest::gtest_main
)

gtest_discover_tests(
    repaint_clock_test
    TEST_PREFIX "repaint_clock."
)

# TUI terminal driver: owns termios/raw-mode and the Maya render/write/commit
# boundary. It does not generate frame bytes itself.
add_library(
    my_agent_ui_terminal
    src/ui/terminal.cpp
)

target_include_directories(
    my_agent_ui_terminal
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_ui_terminal
    PUBLIC cxx_std_26
)

target_link_libraries(
    my_agent_ui_terminal
    PUBLIC
        my_agent_ui_maya
)

add_executable(
    terminal_test
    tests/terminal_test.cpp
)

target_link_libraries(
    terminal_test
    PRIVATE
        my_agent_ui_terminal
        GTest::gtest_main
)

gtest_discover_tests(
    terminal_test
    TEST_PREFIX "terminal."
)

# ── rag: 文档 RAG 主干（学习线切片 #40~#43）───────────────────────────────
# 叶子模块：不依赖 tool/runtime/ui。教材 = agentty/src/rag/。
# 切片 #40：chunker + tokenize + BM25（词匹配路）。
# 切片 #41：语义路（embed.cpp）+ 混合检索（hybrid.cpp）。
add_library(
    my_agent_rag
    src/rag/bm25.cpp
    src/rag/corpus.cpp
    src/rag/embed.cpp
    src/rag/hybrid.cpp
)

# 接口纯净原则（#41 拍板）：http/json 是 rag 的实现依赖，PRIVATE ——
# 不向 rag_test/rag_bench 等下游传递（rag.hpp 里一个相关 include 都没有）。
target_link_libraries(
    my_agent_rag
    PRIVATE
        my_agent_http
        nlohmann_json::nlohmann_json
)

target_include_directories(
    my_agent_rag
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_rag
    PUBLIC cxx_std_26
)

add_executable(
    rag_test
    tests/rag_test.cpp
)

target_link_libraries(
    rag_test
    PRIVATE
        my_agent_rag
        GTest::gtest_main
        # #41 Red 测试要断言 JSON（拼包/解析纯函数）， PRIVATE 即可
        nlohmann_json::nlohmann_json
)

gtest_discover_tests(
    rag_test
    TEST_PREFIX "rag."
)

# 检索质量基准：已知答案合成（tf×idf top-4 拼查询，gold = 该块自身），
# 覆盖匹配，recall@k / MRR / µs。量具不是断言 —— 不进 ctest，跑一次出报告。
# 也不是测试专用量具那种 tests/ 目标：它是 RAG 模块的常驻仪表，
# #41 的阶梯（BM25-only vs +dense vs +RRF）在这上面长出来。
add_executable(
    rag_bench
    src/rag/bench.cpp
)

target_link_libraries(
    rag_bench
    PRIVATE
        my_agent_rag
        my_agent_http
        nlohmann_json::nlohmann_json
)

# 阶段一演示终点：最小 REPL。只碰 dispatch / run_until_quiescent / model
add_executable(
    my_agent_repl
    src/repl/main.cpp
)

target_link_libraries(
    my_agent_repl
    PRIVATE
        my_agent_async_runtime
        my_agent_provider
        my_agent_prompt
        my_agent_ui_loop
)

# maya rendering smoke test (slice #15). Proves the maya seam links and emits
# bytes. The `maya` target comes from FetchContent above; wired SYSTEM to keep
# maya's header warnings out of our build.
add_executable(
    maya_smoke_test
    tests/maya_smoke_test.cpp
)

target_link_libraries(
    maya_smoke_test
    PRIVATE
        maya
        GTest::gtest_main
)

gtest_discover_tests(
    maya_smoke_test
    TEST_PREFIX "maya_smoke."
)
