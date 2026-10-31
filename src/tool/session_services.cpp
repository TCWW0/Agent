#include "my_agent/tool/session_services.hpp"
#include "calculator.hpp"
#include "list_dir.hpp"
#include "memory_tools.hpp"
#include "my_agent/rag/rag.hpp"
#include "my_agent/runtime/tool_replay.hpp"
#include "my_agent/tool/memory_store.hpp"
#include "my_agent/tool/search_docs.hpp"
#include "my_agent/tool/skills.hpp"
#include "my_agent/tool/tool_runtime.hpp"
#include "read.hpp"
#include "skill_tools.hpp"
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace my_agent::tool {

namespace {
fs::path capture_workspace_root()
{
    std::error_code error;
    fs::path root = fs::current_path(error);
    if(error){
        throw std::runtime_error{
            "fail to capture workspace root: "+
            error.message()
        };
    }

    root = fs::canonical(root,error);
    if (error) {
        throw std::runtime_error{
            "failed to canonicalize workspace root: "+
            error.message()
        };
    }

    return root;
}

} // namespace

[[nodiscard]]
SessionFacts discover_session_facts(){
    SessionFacts facts;
    facts.workspace_root = capture_workspace_root();
    facts.memory_roots = memory::discover_roots();
    facts.skill_roots = skills::discover_roots();
    facts.docs = docs::resolve_config(
                [](const char* name) { return std::getenv(name); },
                std::filesystem::current_path(),
                [](const std::filesystem::path& p) {
                    std::error_code ec;
                    const bool ok = std::filesystem::is_directory(p, ec);
                    return !ec && ok;
                });
    return facts;
}

SessionServices::SessionServices(
    SessionFacts facts,
    rag::EmbedBackend docs_backend,
    rag::TeiRerankTransport docs_rerank_transport)
    : read_roots_{std::make_shared<detail::ReadRoots>(facts.workspace_root)}
    , workspace_root_{read_roots_->workspace_root()}
    , memory_{std::make_shared<memory::MemoryStore>(std::move(facts.memory_roots))}
    , skills_{std::make_shared<skills::SkillEngine>(
          std::move(facts.skill_roots),
          [roots = read_roots_](const fs::path& directory) {
              return roots->add(directory).has_value();
          })}
    , docs_{std::move(facts.docs)}
    , docs_index_{std::make_shared<docs::DocsIndex>()}
    , docs_backend_{std::move(docs_backend)}
{
    (void)skills_->all();
    if (docs_.rerank) {
        docs_rerank_backend_ = rag::make_tei_rerank_backend(
            *docs_.rerank,
            std::move(docs_rerank_transport)
        );
    }
}

const fs::path& SessionServices::workspace_root() const noexcept
{
    return workspace_root_;
}

std::vector<std::string> SessionServices::memory_lines() const
{
    std::vector<std::string> lines;

    for (const memory::Scope scope : {memory::Scope::User,memory::Scope::Project}) {
        for (const memory::Record& record : memory_->load_all(scope)) {
            lines.push_back(memory::render_for_prompt(record));
        }
    }

    return lines;
}

std::string SessionServices::skill_catalog() const
{
    return skills_->catalog_block();
}

std::vector<RegisteredTool> SessionServices::registrations() const
{
    std::vector<RegisteredTool> registrations;
    registrations.reserve(7);
    registrations.push_back(detail::make_calculator_tool());
    registrations.push_back(detail::make_read_tool(read_roots_));
    registrations.push_back(detail::make_list_dir_tool(read_roots_));

    std::vector<RegisteredTool> memory_tools =
        detail::make_memory_tools(memory_);
    registrations.insert(
        registrations.end(),
        std::make_move_iterator(memory_tools.begin()),
        std::make_move_iterator(memory_tools.end()));

    registrations.push_back(detail::make_skill_tool(skills_));

    registrations.push_back(docs::make_search_docs_tool(
        docs_, docs_index_, docs_backend_, docs_rerank_backend_));

    return registrations;
}

std::expected<void, std::string> SessionServices::allow_read_root(
    const fs::path& root)
{
    return read_roots_->add(root);
}

} // namespace my_agent::tool
