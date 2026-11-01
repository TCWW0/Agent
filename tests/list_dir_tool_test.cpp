#include "../src/tool/list_dir.hpp"
#include "../src/tool/read.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

namespace fs = std::filesystem;

class ScopedDirectory {
public:
    explicit ScopedDirectory(fs::path parent = fs::temp_directory_path())
    {
        const auto nonce =
            std::chrono::steady_clock::now().time_since_epoch().count();

        for (unsigned int attempt = 0; attempt < 100; ++attempt) {
            path_ = parent / (
                "my_agent_list_dir_fixture_" + std::to_string(nonce) + "_"
                + std::to_string(attempt));

            std::error_code error;
            if (fs::create_directory(path_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::runtime_error{
                    "failed to create list_dir fixture: " + error.message()};
            }
        }

        throw std::runtime_error{"failed to create unique list_dir fixture"};
    }

    ~ScopedDirectory()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    ScopedDirectory(const ScopedDirectory&) = delete;
    ScopedDirectory& operator=(const ScopedDirectory&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void write_file(const fs::path& path, const std::string& content)
{
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        throw std::runtime_error{"failed to create list_dir file fixture"};
    }
    output << content;
    if (!output) {
        throw std::runtime_error{"failed to write list_dir file fixture"};
    }
}

} // namespace

TEST(ListDirToolTest, ListsImmediateEntriesInStableDirectoryFirstOrder)
{
    ScopedDirectory workspace;
    ASSERT_TRUE(fs::create_directory(workspace.path() / "zeta"));
    write_file(workspace.path() / "alpha.txt", "alpha");
    write_file(workspace.path() / "omega.txt", std::string(1024, 'x'));

    auto roots = std::make_shared<my_agent::tool::detail::ReadRoots>(workspace.path());
    const auto list_dir = my_agent::tool::detail::make_list_dir_tool(std::move(roots));

    const auto result = list_dir.handler(
        nlohmann::json::object(),
        my_agent::tool::ToolExecutionContext{});

    ASSERT_TRUE(result.has_value()) << result.error().render();
    EXPECT_EQ(
        "\"zeta\"/\n"
        "\"alpha.txt\"  5 B\n"
        "\"omega.txt\"  1.0 KiB\n",
        result->text);
}

TEST(ListDirToolTest, ReturnsTypedErrorsForMissingAndNonDirectoryPaths)
{
    ScopedDirectory workspace;
    write_file(workspace.path() / "plain.txt", "not a directory");

    auto roots = std::make_shared<my_agent::tool::detail::ReadRoots>(workspace.path());
    const auto list_dir = my_agent::tool::detail::make_list_dir_tool(std::move(roots));

    const auto missing = list_dir.handler(
        nlohmann::json{{"path", "missing"}},
        my_agent::tool::ToolExecutionContext{});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(my_agent::tool::ErrorKind::NotFound, missing.error().kind);

    const auto regular_file = list_dir.handler(
        nlohmann::json{{"path", "plain.txt"}},
        my_agent::tool::ToolExecutionContext{});
    ASSERT_FALSE(regular_file.has_value());
    EXPECT_EQ(my_agent::tool::ErrorKind::NotADirectory, regular_file.error().kind);
}

TEST(ListDirToolTest, UsesSessionReadRootsAndBlocksSymlinkEscapes)
{
    ScopedDirectory fixture{fs::current_path()};
    const fs::path workspace = fixture.path() / "workspace";
    const fs::path allowed = fixture.path() / "allowed";
    const fs::path denied = fixture.path() / "denied";
    ASSERT_TRUE(fs::create_directory(workspace));
    ASSERT_TRUE(fs::create_directory(allowed));
    ASSERT_TRUE(fs::create_directory(denied));
    write_file(allowed / "inside.txt", "allowed");
    write_file(denied / "secret.txt", "denied");

    std::error_code link_error;
    fs::create_directory_symlink(denied, workspace / "escape", link_error);
    ASSERT_FALSE(link_error) << link_error.message();

    auto roots = std::make_shared<my_agent::tool::detail::ReadRoots>(
        workspace, /*allow_temporary_root=*/false);
    ASSERT_TRUE(roots->add(allowed).has_value());
    const auto list_dir = my_agent::tool::detail::make_list_dir_tool(std::move(roots));

    const auto allowed_result = list_dir.handler(
        nlohmann::json{{"path", allowed.string()}},
        my_agent::tool::ToolExecutionContext{});
    ASSERT_TRUE(allowed_result.has_value()) << allowed_result.error().render();
    EXPECT_EQ("\"inside.txt\"  7 B\n", allowed_result->text);

    for (const fs::path& refused : {denied, fs::path{"escape"}}) {
        const auto result = list_dir.handler(
            nlohmann::json{{"path", refused.string()}},
            my_agent::tool::ToolExecutionContext{});
        ASSERT_FALSE(result.has_value()) << refused;
        EXPECT_EQ(my_agent::tool::ErrorKind::OutOfWorkspace, result.error().kind)
            << refused;
    }
}

TEST(ListDirToolTest, RecursesToDepthWithoutFollowingSymlinksOrHeavyDirectories)
{
    ScopedDirectory workspace;
    ASSERT_TRUE(fs::create_directories(workspace.path() / "build"));
    ASSERT_TRUE(fs::create_directories(workspace.path() / "real" / "nested"));
    write_file(workspace.path() / "build" / "generated.txt", "generated");
    write_file(workspace.path() / "real" / "first.txt", "first");
    write_file(workspace.path() / "real" / "nested" / "second.txt", "second");

    std::error_code link_error;
    fs::create_directory_symlink("real", workspace.path() / "alias", link_error);
    ASSERT_FALSE(link_error) << link_error.message();

    auto roots = std::make_shared<my_agent::tool::detail::ReadRoots>(workspace.path());
    const auto list_dir = my_agent::tool::detail::make_list_dir_tool(std::move(roots));
    const auto result = list_dir.handler(
        nlohmann::json{
            {"recursive", true},
            {"max_depth", 2},
        },
        my_agent::tool::ToolExecutionContext{});

    ASSERT_TRUE(result.has_value()) << result.error().render();
    EXPECT_EQ(
        "\"build\"/  [not expanded]\n"
        "\"real\"/\n"
        "  \"nested\"/\n"
        "  \"first.txt\"  5 B\n"
        "\"alias\" -> \"real\"\n",
        result->text);
    EXPECT_EQ(std::string::npos, result->text.find("generated.txt"));
    EXPECT_EQ(std::string::npos, result->text.find("second.txt"));
}

TEST(ListDirToolTest, EscapesInvalidFilenameBytes)
{
    ScopedDirectory workspace;
    std::string filename{"bad"};
    filename.push_back(static_cast<char>(0xFF));
    filename += ".txt";
    write_file(workspace.path() / fs::path{filename}, "x");

    auto roots = std::make_shared<my_agent::tool::detail::ReadRoots>(workspace.path());
    const auto list_dir = my_agent::tool::detail::make_list_dir_tool(std::move(roots));
    const auto result = list_dir.handler(
        nlohmann::json::object(),
        my_agent::tool::ToolExecutionContext{});

    ASSERT_TRUE(result.has_value()) << result.error().render();
    EXPECT_EQ("\"bad\\xFF.txt\"  1 B\n", result->text);
}

TEST(ListDirToolTest, TruncatesAtCompleteEntryBoundaries)
{
    ScopedDirectory workspace;
    const fs::path entry_limit = workspace.path() / "entry-limit";
    const fs::path byte_limit = workspace.path() / "byte-limit";
    ASSERT_TRUE(fs::create_directory(entry_limit));
    ASSERT_TRUE(fs::create_directory(byte_limit));

    for (int index = 0; index <= 1000; ++index) {
        std::string number = std::to_string(index);
        number.insert(0, 4 - number.size(), '0');
        ASSERT_TRUE(fs::create_directory(entry_limit / ("entry-" + number)));
    }
    for (int index = 0; index < 300; ++index) {
        std::string number = std::to_string(index);
        number.insert(0, 3 - number.size(), '0');
        write_file(byte_limit / ("long-" + number + "-" + std::string(220, 'x')), "x");
    }

    auto roots = std::make_shared<my_agent::tool::detail::ReadRoots>(workspace.path());
    const auto list_dir = my_agent::tool::detail::make_list_dir_tool(std::move(roots));

    const auto entry_result = list_dir.handler(
        nlohmann::json{{"path", "entry-limit"}},
        my_agent::tool::ToolExecutionContext{});
    ASSERT_TRUE(entry_result.has_value()) << entry_result.error().render();
    EXPECT_LE(entry_result->text.size(), 64U * 1024U);
    EXPECT_TRUE(entry_result->text.ends_with(
        "[listing truncated after 1000 entries; use a narrower path or glob]\n"));
    EXPECT_EQ(
        1000,
        std::count(entry_result->text.begin(), entry_result->text.end(), '/'));
    EXPECT_EQ(std::string::npos, entry_result->text.find("\"entry-1000\"/"));

    const auto byte_result = list_dir.handler(
        nlohmann::json{{"path", "byte-limit"}},
        my_agent::tool::ToolExecutionContext{});
    ASSERT_TRUE(byte_result.has_value()) << byte_result.error().render();
    EXPECT_LE(byte_result->text.size(), 64U * 1024U);
    const std::string marker =
        "[listing truncated at 64 KiB; use a narrower path or glob]\n";
    ASSERT_TRUE(byte_result->text.ends_with(marker));
    const std::size_t marker_position = byte_result->text.size() - marker.size();
    ASSERT_GT(marker_position, 0U);
    EXPECT_EQ('\n', byte_result->text[marker_position - 1]);
}

TEST(ListDirToolTest, DescriptorExplainsRecursiveArgumentRules)
{
    ScopedDirectory workspace;
    auto roots = std::make_shared<my_agent::tool::detail::ReadRoots>(workspace.path());
    const auto list_dir = my_agent::tool::detail::make_list_dir_tool(std::move(roots));

    EXPECT_EQ("list_dir", list_dir.descriptor.name.value);
    EXPECT_TRUE(list_dir.descriptor.effects.has(my_agent::tool::Effect::ReadFs));
    const auto& schema = list_dir.descriptor.input_schema;
    EXPECT_EQ(false, schema.at("additionalProperties"));
    const auto& properties = schema.at("properties");
    ASSERT_TRUE(properties.at("path").contains("description"));
    EXPECT_EQ(1, properties.at("path").at("minLength"));
    ASSERT_TRUE(properties.at("recursive").contains("description"));
    ASSERT_TRUE(properties.at("max_depth").contains("description"));
    EXPECT_NE(
        std::string::npos,
        properties.at("max_depth").at("description").get<std::string>().find(
            "recursive=true"));
}
