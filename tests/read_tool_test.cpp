#include "../src/tool/read.hpp"
#include "my_agent/tool/session_services.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

namespace fs = std::filesystem;

class ScopedDirectory {
public:
    explicit ScopedDirectory(const fs::path& parent)
    {
        const auto nonce =
            std::chrono::steady_clock::now().time_since_epoch().count();

        for (unsigned int attempt = 0; attempt < 100; ++attempt) {
            path_ = parent / (
                "my_agent_read_tool_fixture_"
                + std::to_string(nonce)
                + "_"
                + std::to_string(attempt)
            );

            std::error_code error;
            if (fs::create_directory(path_, error)) {
                return;
            }

TEST(ReadToolTest, ExplicitRangeBypassesOutlineForLargeFile)
{
    ScopedDirectory workspace{fs::current_path()};
    std::string content{"# Project\n"};
    for (int line = 1; line <= 3000; ++line) {
        content += "ordinary filler line " + std::to_string(line) + "\n";
    }
    content += "class Widget {\npublic:\n    void run();\n};\n";
    ScopedTextFile fixture{workspace.path() / "large.cpp", content};
    const auto read = my_agent::tool::detail::make_read_tool(workspace.path());

    const auto result = read.handler(
        nlohmann::json{
            {"path", "large.cpp"},
            {"start_line", 3002},
            {"end_line", 3004},
        },
        my_agent::tool::ToolExecutionContext{}
    );

    ASSERT_TRUE(result.has_value()) << result.error().render();
    EXPECT_TRUE(result->text.starts_with(
        "class Widget {\npublic:\n    void run();\n")) << result->text;
    EXPECT_EQ(std::string::npos, result->text.find("# Outline"));
}

TEST(ReadToolTest, ReturnsPreviewForLargeFileWithoutRecognizableStructure)
{
    ScopedDirectory workspace{fs::current_path()};
    std::string content;
    for (int line = 1; line <= 3000; ++line) {
        content += "plain data row " + std::to_string(line) + "\n";
    }
    ASSERT_GT(content.size(), 32U * 1024U);
    ScopedTextFile fixture{workspace.path() / "large.log", content};
    const auto read = my_agent::tool::detail::make_read_tool(workspace.path());

    const auto result = read.handler(
        nlohmann::json{{"path", "large.log"}},
        my_agent::tool::ToolExecutionContext{}
    );

    ASSERT_TRUE(result.has_value()) << result.error().render();
    EXPECT_NE(std::string::npos, result->text.find("# First 1 KiB of large.log"));
    EXPECT_NE(std::string::npos, result->text.find("plain data row 1\n"));
    EXPECT_EQ(std::string::npos, result->text.find("plain data row 3000\n"));
    EXPECT_NE(std::string::npos, result->text.find("offset/limit"));
}

TEST(ReadToolTest, ChecksFileSizeBeforeBinaryContent)
{
    ScopedDirectory workspace{fs::current_path()};
    constexpr auto one_mib = 1024U * 1024U;
    std::string content(one_mib + 1U, 'x');
    content.front() = '\0';
    ScopedTextFile fixture{workspace.path() / "oversized.dat", content};
    const auto read = my_agent::tool::detail::make_read_tool(workspace.path());

    const auto result = read.handler(
        nlohmann::json{{"path", "oversized.dat"}},
        my_agent::tool::ToolExecutionContext{}
    );

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(my_agent::tool::ErrorKind::TooLarge, result.error().kind);
}

TEST(ReadToolTest, ReturnsOutlineForLargeFileWithoutExplicitRange)
{
    ScopedDirectory workspace{fs::current_path()};
    std::string content{"# Project\n"};
    for (int line = 1; line <= 3000; ++line) {
        content += "ordinary filler line " + std::to_string(line) + "\n";
    }
    content += "class Widget {\npublic:\n    void run();\n};\n";
    ASSERT_GT(content.size(), 32U * 1024U);
    ScopedTextFile fixture{workspace.path() / "large.cpp", content};
    const auto read = my_agent::tool::detail::make_read_tool(workspace.path());

    const auto result = read.handler(
        nlohmann::json{{"path", "large.cpp"}},
        my_agent::tool::ToolExecutionContext{}
    );

    ASSERT_TRUE(result.has_value()) << result.error().render();
    EXPECT_NE(std::string::npos, result->text.find("# Outline of large.cpp"));
    EXPECT_NE(std::string::npos, result->text.find("[L1] # Project"));
    EXPECT_NE(
        std::string::npos,
        result->text.find("[L3002] class Widget {")
    );
    EXPECT_EQ(
        std::string::npos,
        result->text.find("ordinary filler line 1")
    );
    EXPECT_NE(std::string::npos, result->text.find("offset/limit"));
}

TEST(ReadToolTest, SupportsStartLineAndEndLine)
{
    ScopedDirectory workspace{fs::current_path()};
    ScopedTextFile fixture{
        workspace.path() / "lines.txt",
        "one\ntwo\nthree\nfour\nfive\nsix\n"
    };

    const auto read = my_agent::tool::detail::make_read_tool(workspace.path());
    const auto result = read.handler(
        nlohmann::json{
            {"path", "lines.txt"},
            {"start_line", 2},
            {"end_line", 4},
        },
        my_agent::tool::ToolExecutionContext{}
    );

    ASSERT_TRUE(result.has_value()) << result.error().render();
    EXPECT_TRUE(result->text.starts_with("two\nthree\nfour\n"))
        << result->text;
    EXPECT_EQ(std::string::npos, result->text.find("one\n"));
    EXPECT_EQ(std::string::npos, result->text.find("five\n"));
}

TEST(ReadToolTest, RejectsConflictingRangeAliases)
{
    ScopedDirectory workspace{fs::current_path()};
    ScopedTextFile fixture{
        workspace.path() / "lines.txt",
        "one\ntwo\nthree\nfour\n"
    };
    const auto read = my_agent::tool::detail::make_read_tool(workspace.path());

    const auto conflicting_start = read.handler(
        nlohmann::json{
            {"path", "lines.txt"},
            {"offset", 2},
            {"start_line", 3},
        },
        my_agent::tool::ToolExecutionContext{}
    );
    ASSERT_FALSE(conflicting_start.has_value());
    EXPECT_EQ(
        my_agent::tool::ErrorKind::InvalidArgs,
        conflicting_start.error().kind
    );

    const auto conflicting_end = read.handler(
        nlohmann::json{
            {"path", "lines.txt"},
            {"offset", 2},
            {"limit", 2},
            {"end_line", 4},
        },
        my_agent::tool::ToolExecutionContext{}
    );
    ASSERT_FALSE(conflicting_end.has_value());
    EXPECT_EQ(
        my_agent::tool::ErrorKind::InvalidArgs,
        conflicting_end.error().kind
    );
}

TEST(ReadToolTest, RejectsInvalidRangeArguments)
{
    ScopedDirectory workspace{fs::current_path()};
    const auto read = my_agent::tool::detail::make_read_tool(workspace.path());

    for (const nlohmann::json& args : {
        nlohmann::json{{"path", "missing.txt"}, {"offset", 0}},
        nlohmann::json{{"path", "missing.txt"}, {"limit", -1}},
        nlohmann::json{{"path", "missing.txt"}, {"start_line", "two"}},
        nlohmann::json{
            {"path", "missing.txt"},
            {"start_line", 4},
            {"end_line", 3},
        },
    }) {
        const auto result = read.handler(
            args,
            my_agent::tool::ToolExecutionContext{}
        );
        ASSERT_FALSE(result.has_value()) << args.dump();
        EXPECT_EQ(my_agent::tool::ErrorKind::InvalidArgs, result.error().kind)
            << args.dump();
    }
}

TEST(ReadToolTest, DefaultsToAtMostTwoThousandLines)
{
    ScopedDirectory workspace{fs::current_path()};
    std::string content;
    for (int line = 1; line <= 2001; ++line) {
        content += "line-" + std::to_string(line) + "\n";
    }
    ScopedTextFile fixture{workspace.path() / "many-lines.txt", content};
    const auto read = my_agent::tool::detail::make_read_tool(workspace.path());

    const auto result = read.handler(
        nlohmann::json{{"path", "many-lines.txt"}},
        my_agent::tool::ToolExecutionContext{}
    );

    ASSERT_TRUE(result.has_value()) << result.error().render();
    EXPECT_NE(std::string::npos, result->text.find("line-2000\n"));
    EXPECT_EQ(std::string::npos, result->text.find("line-2001\n"));
    EXPECT_NE(std::string::npos, result->text.find("showing lines 1-2000"));
}

TEST(ReadToolTest, DescriptorDeclaresRangeArguments)
{
    ScopedDirectory workspace{fs::current_path()};
    const auto read = my_agent::tool::detail::make_read_tool(workspace.path());
    const auto& properties = read.descriptor.input_schema.at("properties");

    for (const std::string_view name : {
        "offset", "limit", "start_line", "end_line"}) {
        ASSERT_TRUE(properties.contains(name)) << name;
        EXPECT_EQ("integer", properties.at(name).at("type")) << name;
        EXPECT_EQ(1, properties.at(name).at("minimum")) << name;
    }
}

TEST(ReadToolTest, ReturnsRequestedOffsetAndLimit)
{
    ScopedDirectory workspace{fs::current_path()};
    ScopedTextFile fixture{
        workspace.path() / "lines.txt",
        "one\ntwo\nthree\nfour\nfive\nsix\n"
    };

    const auto read = my_agent::tool::detail::make_read_tool(workspace.path());
    const auto result = read.handler(
        nlohmann::json{
            {"path", "lines.txt"},
            {"offset", 2},
            {"limit", 3},
        },
        my_agent::tool::ToolExecutionContext{}
    );

    ASSERT_TRUE(result.has_value()) << result.error().render();
    EXPECT_TRUE(result->text.starts_with("two\nthree\nfour\n"))
        << result->text;
    EXPECT_EQ(std::string::npos, result->text.find("one\n"));
    EXPECT_EQ(std::string::npos, result->text.find("five\n"));
}

TEST(ReadToolTest, RejectsOversizedFileAsTooLarge)
{
    ScopedDirectory workspace{fs::current_path()};
    constexpr auto one_mib = 1024U * 1024U;
    ScopedTextFile fixture{
        workspace.path() / "oversized.txt",
        std::string(one_mib + 1U, 'x')
    };

    const auto read = my_agent::tool::detail::make_read_tool(workspace.path());
    const auto result = read.handler(
        nlohmann::json{{"path", "oversized.txt"}},
        my_agent::tool::ToolExecutionContext{}
    );

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(my_agent::tool::ErrorKind::TooLarge, result.error().kind);
    EXPECT_TRUE(result.error().render().starts_with("[too large] "))
        << result.error().render();
}
            if (error && error != std::errc::file_exists) {
                throw std::runtime_error{
                    "failed to create read tool fixture directory: "
                    + error.message()
                };
            }
        }

        throw std::runtime_error{
            "failed to create a unique read tool fixture directory"
        };
    }

    ~ScopedDirectory()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    ScopedDirectory(const ScopedDirectory&) = delete;
    ScopedDirectory& operator=(const ScopedDirectory&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept
    {
        return path_;
    }

private:
    fs::path path_;
};

class ScopedTextFile {
public:
    ScopedTextFile(fs::path path, const std::string& content)
        : path_(std::move(path))
    {
        if (fs::exists(path_)) {
            throw std::runtime_error{"read tool fixture already exists"};
        }

        std::ofstream output{path_, std::ios::binary | std::ios::trunc};
        if (!output) {
            throw std::runtime_error{"failed to create read tool fixture"};
        }

        output << content;
        if (!output) {
            std::error_code error;
            fs::remove(path_, error);
            throw std::runtime_error{"failed to write read tool fixture"};
        }
    }

    ~ScopedTextFile()
    {
        std::error_code error;
        fs::remove(path_, error);
    }

    ScopedTextFile(const ScopedTextFile&) = delete;
    ScopedTextFile& operator=(const ScopedTextFile&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept
    {
        return path_;
    }

private:
    fs::path path_;
};

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const fs::path& path)
        : original_{fs::current_path()}
    {
        fs::current_path(path);
    }

    ~ScopedCurrentPath()
    {
        std::error_code error;
        fs::current_path(original_, error);
    }

    ScopedCurrentPath(const ScopedCurrentPath&) = delete;
    ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

private:
    fs::path original_;
};

} // namespace

TEST(ReadToolTest, RegisteredReadToolReadsSmallTextFileInsideWorkspace)
{
    ScopedDirectory fixture_directory{fs::current_path()};
    ScopedTextFile fixture{
        fixture_directory.path() / "inside.txt",
        "alpha\nbeta\n"
    };

    const auto read = my_agent::tool::detail::make_read_tool(
        fs::canonical(fs::current_path()));
    EXPECT_EQ("read", read.descriptor.name.value);
    EXPECT_TRUE(read.descriptor.effects.has(my_agent::tool::Effect::ReadFs));

    const auto result = read.handler(
        nlohmann::json{{
            "path",
            fixture.path().lexically_relative(fs::current_path()).string(),
        }},
        my_agent::tool::ToolExecutionContext{}
    );

    ASSERT_TRUE(result.has_value()) << result.error().render();
    EXPECT_EQ("alpha\nbeta\n", result->text);
}

TEST(ReadToolTest, RejectsExistingFileOutsideWorkspace)
{
    const fs::path workspace_root = fs::canonical(fs::current_path());
    const fs::path outside_directory = workspace_root.parent_path();
    ASSERT_NE(workspace_root, outside_directory);

    ScopedDirectory fixture_directory{outside_directory};
    ScopedTextFile fixture{
        fixture_directory.path() / "outside.txt",
        "outside workspace content\n"
    };

    const auto read = my_agent::tool::detail::make_read_tool(workspace_root);
    const auto result = read.handler(
        nlohmann::json{{"path", fixture.path().string()}},
        my_agent::tool::ToolExecutionContext{});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        my_agent::tool::ErrorKind::OutOfWorkspace,
        result.error().kind
    );
}

TEST(ReadToolTest, RejectsMissingPathOutsideWorkspace)
{
    const fs::path workspace_root = fs::canonical(fs::current_path());
    const fs::path outside_directory = workspace_root.parent_path();
    ASSERT_NE(workspace_root, outside_directory);

    ScopedDirectory fixture_directory{outside_directory};
    const fs::path missing_path = fixture_directory.path() / "missing.txt";
    ASSERT_FALSE(fs::exists(missing_path));

    const auto read = my_agent::tool::detail::make_read_tool(workspace_root);
    const auto result = read.handler(
        nlohmann::json{{"path", missing_path.string()}},
        my_agent::tool::ToolExecutionContext{});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        my_agent::tool::ErrorKind::OutOfWorkspace,
        result.error().kind
    );
}

TEST(ReadToolTest, ExistingRegistrationObservesSessionReadRootAddition)
{
    ScopedDirectory fixture_directory{fs::current_path()};
    const fs::path workspace = fixture_directory.path() / "workspace";
    const fs::path shared = fixture_directory.path() / "shared";
    fs::create_directories(workspace);
    fs::create_directories(shared);
    ScopedTextFile fixture{shared / "note.txt", "shared content\n"};

    my_agent::tool::SessionServices services{
        my_agent::tool::SessionFacts{.workspace_root = workspace}};
    const auto registrations = services.registrations();
    const auto read = std::ranges::find_if(
        registrations,
        [](const my_agent::tool::RegisteredTool& registration) {
            return registration.descriptor.name.value == "read";
        });
    ASSERT_NE(registrations.end(), read);
    const nlohmann::json args{{"path", fixture.path().string()}};

    const auto denied = read->handler(args, my_agent::tool::ToolExecutionContext{});
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(my_agent::tool::ErrorKind::OutOfWorkspace, denied.error().kind);

    const auto added = services.allow_read_root(shared);
    ASSERT_TRUE(added.has_value());
    const auto allowed = read->handler(args, my_agent::tool::ToolExecutionContext{});
    ASSERT_TRUE(allowed.has_value()) << allowed.error().render();
    EXPECT_EQ("shared content\n", allowed->text);
}

TEST(ReadToolTest, SessionReadAllowsTmpByDefault)
{
    ScopedDirectory workspace{fs::current_path()};
    ScopedDirectory temporary{fs::temp_directory_path()};
    ScopedTextFile fixture{temporary.path() / "note.txt", "temporary note\n"};
    const auto read = my_agent::tool::detail::make_read_tool(workspace.path());

    const auto result = read.handler(
        nlohmann::json{{"path", fixture.path().string()}},
        my_agent::tool::ToolExecutionContext{});

    ASSERT_TRUE(result.has_value()) << result.error().render();
    EXPECT_EQ("temporary note\n", result->text);
}

TEST(ReadToolTest, RelativeWorkspaceRootIsFixedWhenRegistrationIsCreated)
{
    ScopedDirectory fixture_directory{fs::current_path()};
    ScopedTextFile fixture{fixture_directory.path() / "note.txt", "workspace note\n"};
    const auto read = my_agent::tool::detail::make_read_tool(
        fixture_directory.path().lexically_relative(fs::current_path()));
    const fs::path elsewhere = fixture_directory.path() / "elsewhere";
    fs::create_directories(elsewhere);
    const ScopedCurrentPath changed_cwd{elsewhere};

    const auto result = read.handler(
        nlohmann::json{{"path", "note.txt"}},
        my_agent::tool::ToolExecutionContext{});

    ASSERT_TRUE(result.has_value()) << result.error().render();
    EXPECT_EQ("workspace note\n", result->text);
}

TEST(ReadToolTest, MissingPathArgumentReturnsInvalidArgs)
{
    ScopedDirectory workspace{fs::current_path()};
    const auto read = my_agent::tool::detail::make_read_tool(workspace.path());

    const auto result = read.handler(
        nlohmann::json::object(),
        my_agent::tool::ToolExecutionContext{});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(my_agent::tool::ErrorKind::InvalidArgs, result.error().kind);
    EXPECT_EQ("read requires a string path", result.error().message);
}

TEST(ReadToolTest, MissingFileInsideWorkspaceReturnsNotFound)
{
    ScopedDirectory workspace{fs::current_path()};
    const auto read = my_agent::tool::detail::make_read_tool(workspace.path());

    const auto result = read.handler(
        nlohmann::json{{"path", "missing.txt"}},
        my_agent::tool::ToolExecutionContext{});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(my_agent::tool::ErrorKind::NotFound, result.error().kind);
}

TEST(ReadToolTest, InvalidExtraRootDoesNotWidenSessionAccess)
{
    ScopedDirectory fixture_directory{fs::current_path()};
    const fs::path workspace = fixture_directory.path() / "workspace";
    fs::create_directories(workspace);
    ScopedTextFile fixture{fixture_directory.path() / "secret.txt", "secret\n"};
    my_agent::tool::SessionServices services{
        my_agent::tool::SessionFacts{.workspace_root = workspace}};
    const auto registrations = services.registrations();
    const auto read = std::ranges::find_if(
        registrations,
        [](const my_agent::tool::RegisteredTool& registration) {
            return registration.descriptor.name.value == "read";
        });
    ASSERT_NE(registrations.end(), read);

    const auto added = services.allow_read_root(
        fixture_directory.path() / "does-not-exist");
    ASSERT_FALSE(added.has_value());
    EXPECT_NE(std::string::npos, added.error().find("existing directory"));
    const auto result = read->handler(
        nlohmann::json{{"path", fixture.path().string()}},
        my_agent::tool::ToolExecutionContext{});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(my_agent::tool::ErrorKind::OutOfWorkspace, result.error().kind);
}

TEST(ReadToolTest, InvalidWorkspaceRootFailsAtSessionConstruction)
{
    ScopedDirectory fixture_directory{fs::current_path()};
    const fs::path missing = fixture_directory.path() / "missing-workspace";
    EXPECT_THROW(
        (void)my_agent::tool::SessionServices{
            my_agent::tool::SessionFacts{.workspace_root = missing}},
        std::invalid_argument);
}

TEST(ReadToolTest, SymlinkInsideWorkspaceCannotReadOutsideAllowedRoots)
{
    ScopedDirectory fixture_directory{fs::current_path()};
    const fs::path workspace = fixture_directory.path() / "workspace";
    fs::create_directories(workspace);
    ScopedTextFile outside{fixture_directory.path() / "outside.txt", "outside\n"};
    fs::create_symlink(outside.path(), workspace / "escape.txt");
    const auto read = my_agent::tool::detail::make_read_tool(workspace);

    const auto result = read.handler(
        nlohmann::json{{"path", "escape.txt"}},
        my_agent::tool::ToolExecutionContext{});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(my_agent::tool::ErrorKind::OutOfWorkspace, result.error().kind);
}

TEST(ReadToolTest, RejectsDirectoryAsNotAFile)
{
    ScopedDirectory workspace{fs::current_path()};
    const fs::path directory = workspace.path() / "nested";
    ASSERT_TRUE(fs::create_directory(directory));

    const auto read = my_agent::tool::detail::make_read_tool(workspace.path());
    const auto result = read.handler(
        nlohmann::json{{"path", "nested"}},
        my_agent::tool::ToolExecutionContext{}
    );

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(my_agent::tool::ErrorKind::NotAFile, result.error().kind);
    EXPECT_TRUE(result.error().render().starts_with("[not a file] "))
        << result.error().render();
}

TEST(ReadToolTest, RejectsBinaryFileAsBinary)
{
    ScopedDirectory workspace{fs::current_path()};
    ScopedTextFile fixture{
        workspace.path() / "binary.dat",
        std::string{"alpha\0omega", 11}
    };

    const auto read = my_agent::tool::detail::make_read_tool(workspace.path());
    const auto result = read.handler(
        nlohmann::json{{"path", "binary.dat"}},
        my_agent::tool::ToolExecutionContext{}
    );

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(my_agent::tool::ErrorKind::Binary, result.error().kind);
    EXPECT_TRUE(result.error().render().starts_with("[binary] "))
        << result.error().render();
}
