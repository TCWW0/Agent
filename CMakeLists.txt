cmake_minimum_required(VERSION 4.4)
project (my_agent
    VERSION 0.0.1
    LANGUAGES CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(
    MY_AGENT_BUILD_TUI
    "Build the Maya-backed terminal UI and the my_agent executable"
    ON
)

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

if(MY_AGENT_BUILD_TUI)
# ── maya: TUI rendering library (slice #15) ─────────────────────────────────
# We take maya ONLY for its renderer. The single seam we use is
#   FrameBuffer::render(const Element&, const Theme&) -> const std::string&
# plus commit(). We deliberately DO NOT use maya's run<Program>() event loop:
# its Program concept demands update(Model, Msg) -> pair<Model, Cmd<Msg>>,
# which would take over this project's core AsyncHost/WorkPool/WakeSignal loop
# — the very thing this project exists to demonstrate. Rendering only; the
# event loop stays ours. Do not reintroduce run<Program>() as a "simplification".
#
# 应用特有的 caret、状态栏、turn 组合与动画宿主接缝都由
# include/my_agent/ui 下的适配器持有，不要求修改 Maya 源码。Maya 以
# submodule 形式固定在仓库内；子模块未初始化时，FetchContent 仍可按
# 同一提交从上游获取。显式的 FETCHCONTENT_SOURCE_DIR_MAYA 始终优先。
#
# Two options MUST be forced off before MakeAvailable:
#   MAYA_BUILD_EXAMPLES — we don't build maya's demos.
#   MAYA_NATIVE_TUNING  — default ON bakes -march=native into the binary,
#                         which would make our build non-portable.
# (maya's tests/fuzzers already default OFF.)
set(MAYA_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(MAYA_NATIVE_TUNING  OFF CACHE INTERNAL "")
set(_my_agent_local_maya "${CMAKE_CURRENT_SOURCE_DIR}/maya")
if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_MAYA
   AND EXISTS "${_my_agent_local_maya}/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_MAYA "${_my_agent_local_maya}")
endif()
FetchContent_Declare(
    maya
    GIT_REPOSITORY https://github.com/1ay1/maya.git
    GIT_TAG d3d53262ce6888ff782e130fe174a7bd5da9fc58
    SYSTEM
)
FetchContent_MakeAvailable(maya)
endif()

include(GoogleTest)

enable_testing()

find_package(Threads REQUIRED)

add_library(
    my_agent_tools
    src/tool/list_dir.cpp
    src/tool/read.cpp
    src/tool/read_roots.cpp
    src/tool/tool_protocol.cpp
    src/tool/tool_runtime.cpp
    src/tool/memory_store.cpp
    src/tool/memory_tools.cpp
    src/tool/skills.cpp
    src/tool/skill_tools.cpp
    src/tool/docs_tools.cpp
    src/tool/session_services.cpp
    src/tool/calculator.cpp
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
    src/rag/rerank.cpp
    src/rag/tei_rerank.cpp
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

add_executable(
    tei_rerank_smoke_test
    tests/tei_rerank_smoke_test.cpp
)

target_link_libraries(
    tei_rerank_smoke_test
    PRIVATE
        my_agent_rag
        GTest::gtest_main
)

gtest_discover_tests(
    tei_rerank_smoke_test
    TEST_PREFIX "tei_rerank_smoke."
)

# search_docs 工具（#43）：search_docs.hpp 公共接缝暴露 rag 类型，故
# PUBLIC —— rag 保持叶子、单向无环（rag 不回头依赖 tool）。
target_link_libraries(
    my_agent_tools
    PUBLIC my_agent_rag
)

add_executable(
    search_docs_tool_test
    tests/search_docs_tool_test.cpp
)

target_link_libraries(
    search_docs_tool_test
    PRIVATE
        my_agent_tools
        GTest::gtest_main
)

gtest_discover_tests(
    search_docs_tool_test
    TEST_PREFIX "search_docs."
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
    src/runtime/compaction.cpp
    src/runtime/context.cpp
    src/runtime/generic_unicode_estimator.cpp
    src/runtime/request_preparation.cpp
    src/runtime/usage.cpp
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

add_executable(
    context_admission_test
    tests/context_admission_test.cpp
)

target_link_libraries(
    context_admission_test
    PRIVATE
        my_agent_core
        GTest::gtest_main
)

gtest_discover_tests(
    context_admission_test
    TEST_PREFIX "context_admission."
)

add_executable(
    context_maintenance_test
    tests/context_maintenance_test.cpp
)

target_link_libraries(
    context_maintenance_test
    PRIVATE
        my_agent_async_runtime
        GTest::gtest_main
)

gtest_discover_tests(
    context_maintenance_test
    TEST_PREFIX "context_maintenance."
)

add_executable(
    compaction_test
    tests/compaction_test.cpp
)

target_link_libraries(
    compaction_test
    PRIVATE
        my_agent_core
        GTest::gtest_main
)

gtest_discover_tests(
    compaction_test
    TEST_PREFIX "compaction."
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
        my_agent_prompt
        GTest::gtest_main
)

gtest_discover_tests(
    async_host_test
    TEST_PREFIX "async_host."
)

add_executable(
    ts005_resolved_stream_calls_test
    tests/ts005_resolved_stream_calls_test.cpp
)

target_link_libraries(
    ts005_resolved_stream_calls_test
    PRIVATE
        my_agent_async_runtime
        GTest::gtest_main
)

gtest_discover_tests(
    ts005_resolved_stream_calls_test
    TEST_PREFIX "ts005."
)

add_executable(
    tool_loop_test
    tests/tool_loop_test.cpp
)

target_link_libraries(
    tool_loop_test
    PRIVATE
        my_agent_async_runtime
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
    tool_effects_test
    tests/tool_effects_test.cpp
)

target_link_libraries(
    tool_effects_test
    PRIVATE
        my_agent_tools
        GTest::gtest_main
)

gtest_discover_tests(
    tool_effects_test
    TEST_PREFIX "tool_effects."
)

add_executable(
    tool_runtime_test
    tests/tool_runtime_test.cpp
)

target_link_libraries(
    tool_runtime_test
    PRIVATE
        my_agent_tools
        GTest::gtest_main
)

gtest_discover_tests(
    tool_runtime_test
    TEST_PREFIX "tool_runtime."
)

add_executable(
    session_services_test
    tests/session_services_test.cpp
)

target_link_libraries(
    session_services_test
    PRIVATE
        my_agent_tools
        GTest::gtest_main
)

gtest_discover_tests(
    session_services_test
    TEST_PREFIX "session_services."
)

add_executable(
    ts007_native_registration_contract_test
    tests/ts007_native_registration_contract_test.cpp
)

target_link_libraries(
    ts007_native_registration_contract_test
    PRIVATE
        my_agent_tools
        GTest::gtest_main
)

gtest_discover_tests(
    ts007_native_registration_contract_test
    TEST_PREFIX "ts007_native_registration_contract."
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
    list_dir_tool_test
    tests/list_dir_tool_test.cpp
)
target_link_libraries(
    list_dir_tool_test
    PRIVATE
        my_agent_tools
        GTest::gtest_main
)
gtest_discover_tests(
    list_dir_tool_test
    TEST_PREFIX "list_dir_tool."
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
    src/provider/error.cpp
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
    ts006_tool_execution_report_test
    tests/ts006_tool_execution_report_test.cpp
)

target_link_libraries(
    ts006_tool_execution_report_test
    PRIVATE
        my_agent_async_runtime
        my_agent_provider
        GTest::gtest_main
)

gtest_discover_tests(
    ts006_tool_execution_report_test
    TEST_PREFIX "ts006."
)

add_executable(
    tool_replay_test
    tests/tool_replay_test.cpp
)

target_link_libraries(
    tool_replay_test
    PRIVATE
        my_agent_provider
        GTest::gtest_main
)

gtest_discover_tests(
    tool_replay_test
    TEST_PREFIX "tool_replay."
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

add_executable(
    provider_error_test
    tests/provider_error_test.cpp
)

target_link_libraries(
    provider_error_test
    PRIVATE
        my_agent_provider
        GTest::gtest_main
)

gtest_discover_tests(
    provider_error_test
    TEST_PREFIX "provider_error."
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
if(MY_AGENT_BUILD_TUI)
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

add_executable(
    status_bar_test
    tests/status_bar_test.cpp
)

target_link_libraries(
    status_bar_test
    PRIVATE
        my_agent_ui_screen
        my_agent_virtual_terminal
        GTest::gtest_main
)

gtest_discover_tests(
    status_bar_test
    TEST_PREFIX "status_bar."
)
endif()

# 语义屏幕（issue #26）：Model + UiState → maya widget Config 的纯投影。
# transcript 与 dock 在结构上分离；视觉来自 maya 现成 widget，
# dock 的组合语义归本项目。
add_library(
    my_agent_ui_transcript_guidance
    src/ui/transcript_guidance.cpp
)

target_include_directories(
    my_agent_ui_transcript_guidance
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_ui_transcript_guidance
    PUBLIC cxx_std_26
)

target_link_libraries(
    my_agent_ui_transcript_guidance
    PUBLIC my_agent_core
)

add_executable(
    transcript_guidance_test
    tests/transcript_guidance_test.cpp
)

target_link_libraries(
    transcript_guidance_test
    PRIVATE
        my_agent_ui_transcript_guidance
        GTest::gtest_main
)

gtest_discover_tests(
    transcript_guidance_test
    TEST_PREFIX "transcript_guidance."
)

if(MY_AGENT_BUILD_TUI)
add_library(
    my_agent_ui_screen
    src/ui/screen.cpp
)

target_include_directories(
    my_agent_ui_screen
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_ui_screen
    PUBLIC cxx_std_26
)

target_link_libraries(
    my_agent_ui_screen
    PUBLIC
        my_agent_core
        my_agent_ui_composer_paste
        my_agent_ui_transcript_guidance
        maya
)

add_executable(
    screen_test
    tests/screen_test.cpp
)

target_link_libraries(
    screen_test
    PRIVATE
        my_agent_ui_screen
        GTest::gtest_main
)

gtest_discover_tests(
    screen_test
    TEST_PREFIX "screen."
)

# dock 布局（issue #26 红 2）：DockConfig → Element → FrameBuffer →
# VirtualTerminal 网格。布局语义只在渲染后存在，断言钉在网格层。
add_executable(
    dock_layout_test
    tests/dock_layout_test.cpp
)

target_link_libraries(
    dock_layout_test
    PRIVATE
        my_agent_ui_painter
        my_agent_ui_screen
        my_agent_virtual_terminal
        GTest::gtest_main
)

gtest_discover_tests(
    dock_layout_test
    TEST_PREFIX "dock_layout."
)

# caret 接缝（issue #26 红 3）：caret 必须是**画出来的一格**，而不是硬件光标
# 元数据；不得挤动正文，并且要跨两次成功提交与同一 FrameBuffer 的 resize 保持
# 实心。断言落在 VirtualTerminal 的格子上（文本 + 样式）。
add_executable(
    caret_seam_test
    tests/caret_seam_test.cpp
)

target_link_libraries(
    caret_seam_test
    PRIVATE
        my_agent_ui_screen
        my_agent_virtual_terminal
        GTest::gtest_main
)

gtest_discover_tests(
    caret_seam_test
    TEST_PREFIX "caret_seam."
)
endif()

# TUI 事件循环：apply_key 是纯的（按键 -> UiState 变更 + 可选 Msg），
# run_ui 才碰 poll 与终端。分开是为了让按键语义脱离 tty 可测。
if(MY_AGENT_BUILD_TUI)
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
        my_agent_ui_transcript_guidance
        my_agent_ui_clock
        my_agent_ui_input_editor
        my_agent_ui_input
        my_agent_ui_composer_paste
        my_agent_ui_config
        my_agent_ui_painter
        my_agent_ui_terminal_session
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
        my_agent_virtual_terminal
        GTest::gtest_main
)

gtest_discover_tests(
    ui_loop_test
    TEST_PREFIX "ui_loop."
)
endif()

add_library(
    my_agent_ui_config
    src/ui/ui_config.cpp
)

target_include_directories(
    my_agent_ui_config
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_ui_config
    PUBLIC cxx_std_26
)

add_executable(
    ui_config_test
    tests/ui_config_test.cpp
)

target_link_libraries(
    ui_config_test
    PRIVATE
        my_agent_ui_config
        GTest::gtest_main
)

gtest_discover_tests(
    ui_config_test
    TEST_PREFIX "ui_config."
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

add_library(
    my_agent_ui_composer_paste
    src/ui/composer_paste.cpp
)

target_include_directories(
    my_agent_ui_composer_paste
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_ui_composer_paste
    PUBLIC cxx_std_26
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
        my_agent_ui_composer_paste
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

# TUI 帧率合并：纯判定，时间由调用方注入，因此可确定性测试而无需 sleep。
add_library(
    my_agent_ui_clock
    src/ui/repaint_clock.cpp
    src/ui/stream_rate.cpp
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

add_executable(
    stream_rate_test
    tests/stream_rate_test.cpp
)

target_link_libraries(
    stream_rate_test
    PRIVATE
        my_agent_ui_clock
        GTest::gtest_main
)

gtest_discover_tests(
    stream_rate_test
    TEST_PREFIX "stream_rate."
)

# TUI terminal session and painter are separate targets so the dependency graph
# makes the terminal/rendering boundary explicit.
add_library(
    my_agent_ui_terminal_session
    src/ui/terminal_session.cpp
)

target_include_directories(
    my_agent_ui_terminal_session
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_ui_terminal_session
    PUBLIC cxx_std_26
)

if(MY_AGENT_BUILD_TUI)
add_library(
    my_agent_ui_transcript_viewport
    src/ui/transcript_viewport.cpp
)

target_include_directories(
    my_agent_ui_transcript_viewport
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_ui_transcript_viewport
    PUBLIC cxx_std_26
)

target_link_libraries(
    my_agent_ui_transcript_viewport
    PUBLIC
        maya
)

add_executable(
    transcript_viewport_test
    tests/transcript_viewport_test.cpp
)

target_link_libraries(
    transcript_viewport_test
    PRIVATE
        my_agent_ui_transcript_viewport
        GTest::gtest_main
)

gtest_discover_tests(
    transcript_viewport_test
    TEST_PREFIX "transcript_viewport."
)

add_library(
    my_agent_ui_painter
    src/ui/painter.cpp
)

target_include_directories(
    my_agent_ui_painter
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(
    my_agent_ui_painter
    PUBLIC cxx_std_26
)

target_link_libraries(
    my_agent_ui_painter
    PUBLIC
        my_agent_ui_screen
        my_agent_ui_transcript_viewport
)

add_executable(
    terminal_test
    tests/terminal_test.cpp
)

target_link_libraries(
    terminal_test
    PRIVATE
        my_agent_ui_painter
        my_agent_ui_terminal_session
        my_agent_virtual_terminal
        GTest::gtest_main
)

gtest_discover_tests(
    terminal_test
    TEST_PREFIX "terminal."
)
endif()

add_executable(
    terminal_session_test
    tests/terminal_session_test.cpp
)

target_link_libraries(
    terminal_session_test
    PRIVATE
        my_agent_ui_terminal_session
        GTest::gtest_main
)

gtest_discover_tests(
    terminal_session_test
    TEST_PREFIX "terminal_session."
)

if(MY_AGENT_BUILD_TUI)
add_executable(
    painter_test
    tests/painter_test.cpp
)

target_link_libraries(
    painter_test
    PRIVATE
        my_agent_ui_painter
        my_agent_virtual_terminal
        GTest::gtest_main
)

gtest_discover_tests(
    painter_test
    TEST_PREFIX "painter."
)
endif()

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

# 人工 passage judgments 的学习型质量阶梯。与 Agentty 的 rag-bench 一样，
# 它是只出报告、不设质量阈值的量具，因此刻意不 add_test；后续每次检索
# 实验都用同一批 query 比较 bm25-only → retrieval → +local-rerank。
add_executable(
    rag_quality_ladder
    tests/rag_quality_ladder.cpp
)

target_link_libraries(
    rag_quality_ladder
    PRIVATE
        my_agent_rag
        my_agent_http
        nlohmann_json::nlohmann_json
)

add_test(
    NAME rag_bench.judgments_reports_hit_at_one
    COMMAND ${CMAKE_COMMAND}
        -DRAG_BENCH=$<TARGET_FILE:rag_bench>
        -DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/rag_bench_judgments
        -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/rag_bench_judgments_test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/rag_bench_judgments_test.cmake
)

set_tests_properties(
    rag_bench.judgments_reports_hit_at_one
    PROPERTIES TIMEOUT 15
)

add_test(
    NAME rag_bench.judgments_reports_each_case
    COMMAND ${CMAKE_COMMAND}
        -DRAG_BENCH=$<TARGET_FILE:rag_bench>
        -DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/rag_bench_case_report
        -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/rag_bench_case_report_test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/rag_bench_case_report_test.cmake
)

set_tests_properties(
    rag_bench.judgments_reports_each_case
    PROPERTIES TIMEOUT 15
)

add_test(
    NAME rag_bench.judgments_rejects_invalid_passages
    COMMAND ${CMAKE_COMMAND}
        -DRAG_BENCH=$<TARGET_FILE:rag_bench>
        -DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/rag_bench_invalid_judgments
        -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/rag_bench_judgments_validation_test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/rag_bench_judgments_validation_test.cmake
)

set_tests_properties(
    rag_bench.judgments_rejects_invalid_passages
    PROPERTIES TIMEOUT 15
)

add_test(
    NAME rag_bench.judgments_honors_candidate_budget
    COMMAND ${CMAKE_COMMAND}
        -DRAG_BENCH=$<TARGET_FILE:rag_bench>
        -DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/rag_bench_case_report
        -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/rag_bench_candidate_budget_test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/rag_bench_candidate_budget_test.cmake
)

set_tests_properties(
    rag_bench.judgments_honors_candidate_budget
    PROPERTIES TIMEOUT 15
)

# 阶段一演示终点：最小 REPL。只碰 dispatch / run_until_quiescent / model
if(MY_AGENT_BUILD_TUI)
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
endif()

# maya rendering smoke test (slice #15). Proves the maya seam links and emits
# bytes. The `maya` target comes from FetchContent above; wired SYSTEM to keep
# maya's header warnings out of our build.
if(MY_AGENT_BUILD_TUI)
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
endif()

# Architectural boundary: the project owns its AsyncHost/event loop and only
# consumes Maya's renderer/core seams. Keep this as a normal CTest entry so a
# new direct app/umbrella/run dependency fails with the offending path.
add_test(
    NAME maya.boundary
    COMMAND ${CMAKE_COMMAND}
        -DPROJECT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/maya_boundary.cmake
)

if(MY_AGENT_BUILD_TUI)
    gtest_discover_tests(
        maya_smoke_test
        TEST_PREFIX "maya_smoke."
    )
endif()
