#pragma once

#include "my_agent/runtime/model.hpp"
#include "my_agent/runtime/msg.hpp"
#include "my_agent/tool/tool_protocol.hpp"

#include <optional>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>

namespace my_agent{
    // agent 核心希望交付给外部的信息，外部可以使用这个结构体中的信息来继续操作
    struct StartStream{
        StreamRef ref{};
        Request request;
        RequestFingerprint request_fingerprint{};
    };

    struct PrepareRequest {
        PreparationId preparation_id{};
        Request request;
        RequestPolicy policy;
        ContextProjectionLedger ledger;
    };

    struct StartCompaction {
        CompactionId compaction_id{};
        Request request;
        RequestFingerprint request_fingerprint{};
    };

    struct RunTool{
        ToolCallRef call{};
        tool::ToolRef ref{};
        nlohmann::json args;
    };

    struct CancelOperation {
        OperationRef operation;
    };

    struct NoCommand{};

    // 从Agent中流转出来的信息，外界后续将只面对这个结构
    using Cmd = std::variant<NoCommand,PrepareRequest,StartCompaction,
        StartStream,RunTool,CancelOperation>;

    // 语义是 新的内部状态+需要执行的外部指令
    struct Step{
        Model model;
        Cmd cmd;
    };

    // 根据一个 Msg 生成后继 Model 和需要执行的 Cmd。
    // Model 按值传入：调用方可以复制旧快照，也可以通过 std::move()
    // 转移状态所有权。update() 不保留对输入 Model 或 Msg 的引用。
    [[nodiscard]]
    Step update(Model model,Msg msg);

}// namespace my_agent
