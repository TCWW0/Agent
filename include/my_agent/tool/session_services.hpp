#pragma once

#include "my_agent/rag/rag.hpp"
#include "my_agent/tool/memory_store.hpp"
#include "my_agent/tool/search_docs.hpp"
#include "my_agent/tool/skills.hpp"
#include "my_agent/tool/tool_runtime.hpp"
#include <filesystem>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace my_agent::tool {
namespace fs = std::filesystem;
namespace detail { class ReadRoots; }

struct SessionFacts {
    fs::path workspace_root;
    memory::Roots memory_roots;
    std::vector<fs::path> skill_roots;
    docs::DocsConfig docs;
};

[[nodiscard]]
SessionFacts discover_session_facts();

class SessionServices {
public:
    explicit SessionServices(
        SessionFacts facts,
        rag::EmbedBackend docs_backend = rag::ollama_embed,
        rag::TeiRerankTransport docs_rerank_transport = {});

    SessionServices(const SessionServices&) = delete;
    SessionServices& operator= (const SessionServices&) = delete;

    [[nodiscard]]
    const fs::path& workspace_root() const noexcept;

    [[nodiscard]]
    std::vector<std::string> memory_lines() const;

    [[nodiscard]]
    std::string skill_catalog() const;

    [[nodiscard]]
    std::vector<RegisteredTool> registrations() const;

    // 只扩展本会话 read 的允许目录。无效目录返回错误，不改变已有权限。
    [[nodiscard]]
    std::expected<void, std::string> allow_read_root(const fs::path& root);

private:
    std::shared_ptr<detail::ReadRoots> read_roots_;
    fs::path workspace_root_;
    std::shared_ptr<memory::MemoryStore> memory_;
    std::shared_ptr<skills::SkillEngine> skills_;
    docs::DocsConfig docs_;
    std::shared_ptr<docs::DocsIndex> docs_index_;
    rag::EmbedBackend docs_backend_;
    rag::RerankBackend docs_rerank_backend_;
};

} // namespace my_agent::tool
