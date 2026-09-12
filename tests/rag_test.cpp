// 切片①（issue #40）Red 测试 —— 内容由你按交付清单亲手填入本文件。
// 契约见 include/my_agent/rag/rag.hpp；教材见 agentty/src/rag/bm25.cpp。

#include "my_agent/rag/rag.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <utility>
#include <expected>

namespace{
    namespace rag = my_agent::rag;

    rag::Chunk make_chunk(std::string text){
        rag::Chunk c;
        c.path = "doc.md";
        c.line_start = 1;
        c.line_end = 1;
        c.text = std::move(text);
        return c;
    }

    // 简单的分词器
    std::vector<std::string> toks_of(std::string_view s){
        std::vector<std::string> out;
        rag::tokenize(s, out);
        return out;
    }

    // 最小的 utf-8 编码校验器，多字节序列必须完整(没被从中间切开)
    bool is_valid_utf8(std::string_view s){
        std::size_t i = 0;
        while(i<s.size()){
            const auto& b = static_cast<unsigned char>(s[i]);
            std::size_t len;
            if      ((b & 0x80) == 0x00) len = 1;
            else if ((b & 0xE0) == 0xC0) len = 2;
            else if ((b & 0xF0) == 0xE0) len = 3;
            else if ((b & 0xF8) == 0xF0) len = 4;
            else return false;                      // 孤立的延续字节
            if (i+len>s.size()) return false;       // 序列被截断
            for(std::size_t j=1;j<len;j++){
                if((static_cast<unsigned char>(s[i+j])&0xC0)!=0x80)
                    return false;
            }
            i+=len;
        }
        return true;
    }

    // 将整体按照换行符来进行切分
    std::vector<std::string> split_lines(std::string_view body) {
        std::vector<std::string> lines;
        std::size_t start = 0;
        for(std::size_t i=0;i<=body.size();++i){
            if(i == body.size()||body[i] =='\n'){
                lines.emplace_back(body.substr(start,i-start));
                start = i+1;
            }
        }
        return lines;
    }

} //namespace

// ── A. tokenize：分词契约（索引/查询共用的唯一入口）─────────────────────

TEST(Tokenize,LowercaseAlphanumericRuns) {
    EXPECT_EQ(toks_of("The Quick-Brown FOX"),
        (std::vector<std::string>{"the","quick","brown","fox"}));
}

//ASCII 字母/数字组成的普通 token，长度必须至少为 2 才会进入最终 token 列表。
TEST(Tokenize, DropsSingleCharacterTokens) {
    EXPECT_EQ(toks_of("a I am"), (std::vector<std::string>{"am"}));
}

//数字可以出现在 token 中，数字不是天然的分隔符。
TEST(Tokenize, DigitsAreKeptAndShortRunsDropped) {
    EXPECT_EQ(toks_of("k1=1.5"), (std::vector<std::string>{"k1"}));
}

// 中文字符默认按照2字进行切割
TEST(Tokenize, CjkRunBecomesOverlappingBigrams) {
    EXPECT_EQ(toks_of("检索引擎"),
              (std::vector<std::string>{"检索", "索引", "引擎"}));
    EXPECT_EQ(toks_of("注册表"),
              (std::vector<std::string>{"注册", "册表"}));
}

// 当中文字符长度只有1时，此时需要保留为token
TEST(Tokenize, LoneCjkCharIsEmittedAsIs) {
    EXPECT_EQ(toks_of("图"), (std::vector<std::string>{"图"}));
}

// 不同文字体系之间不能混在一起处理
TEST(Tokenize, MixedScriptRunsSplitAtBoundaries) {
    EXPECT_EQ(toks_of("配置BM25参数"),
              (std::vector<std::string>{"配置", "bm25", "参数"}));
}

// 对于极端的标点场景，不切分出token
TEST(Tokenize, PunctuationOnlyAndEmptyProduceNothing) {
    EXPECT_TRUE(toks_of("!!! ... ???").empty());
    EXPECT_TRUE(toks_of("").empty());
}

// ── B. chunker：切块契约（行对齐 + 双上限 + 面包屑 + overlap）────────────
TEST(Chunker,ChunksAreLineAlignedWithConsistentSpans){
    const std::string body =
        "first line\nsecond line\n\nthird line\nfourth line\nfifth line\n";
    const auto lines = split_lines(body);
    const auto chunks = rag::chunk_document("doc.md", body,
                                            /*max_lines=*/3, /*max_chars=*/1000,
                                            /*overlap=*/0);
    ASSERT_FALSE(chunks.empty());
    // span 内的源行逐字拼起来（每行带 '\n'）必须等于块的正文。
    for (const auto& c : chunks) {
        ASSERT_GE(c.line_start, 1);
        ASSERT_LE(c.line_end, static_cast<int>(lines.size()));
        ASSERT_LE(c.line_start, c.line_end);
        std::string expected;
        for (int ln = c.line_start; ln <= c.line_end; ++ln)
            expected += lines[ln - 1] + '\n';
        EXPECT_EQ(c.text, expected);
    }
    // 无内容丢失：每个非空白源行都落在某个块里。
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find_first_not_of(" \t\r") == std::string::npos) continue;
        bool found = false;
        for (const auto& c : chunks)
            if (c.line_start <= static_cast<int>(i) + 1
                && static_cast<int>(i) + 1 <= c.line_end) { found = true; break; }
        EXPECT_TRUE(found) << "line " << (i + 1) << " lost";
    }
}

// 限制一个chunk的最大容量限制不被打破
TEST(Chunker, RespectsMaxCharsBound) {
    std::string body;
    for (int i = 0; i < 30; ++i) body += "0123456789xxxxx\n";   // 16 字节/行
    const auto chunks = rag::chunk_document("big.md", body,
                                            /*max_lines=*/100, /*max_chars=*/64,
                                            /*overlap=*/0);
    ASSERT_GT(chunks.size(), 1u);
    for (const auto& c : chunks) {
        std::string_view t = c.text;
        if (!t.empty() && t.back() == '\n') t.remove_suffix(1);  // 容忍行终止符
        EXPECT_LE(t.size(), 64u) << "chunk overruns max_chars";
    }
}

// 优先按行切chunk，如果某一行达到了上限，直接在达到上限时切掉
TEST(Chunker, HardSplitsOversizedLine) {
    const std::string body(5000, 'a');   // 单行 5000 字符，无换行
    const auto chunks = rag::chunk_document("min.js", body,
                                            /*max_lines=*/40, /*max_chars=*/1000,
                                            /*overlap=*/0);
    ASSERT_FALSE(chunks.empty());
    std::size_t total = 0;
    for (const auto& c : chunks) {
        std::string_view t = c.text;
        if (!t.empty() && t.back() == '\n') t.remove_suffix(1);
        EXPECT_LE(t.size(), 1000u);
        total += t.size();
    }
    EXPECT_EQ(total, 5000u);   // 分片无损：拼回原文
}

TEST(Chunker, HardSplitNeverCutsUtf8Codepoint) {
    std::string line;
    for (int i = 0; i < 400; ++i) line += "汉";   // 400 × 3 字节 = 1200 字节
    const auto chunks = rag::chunk_document("u.md", line,
                                            /*max_lines=*/40, /*max_chars=*/500,
                                            /*overlap=*/0);
    ASSERT_FALSE(chunks.empty());
    std::size_t han_count = 0;
    for (const auto& c : chunks) {
        std::string_view t = c.text;
        if (!t.empty() && t.back() == '\n') t.remove_suffix(1);
        EXPECT_TRUE(is_valid_utf8(t));
        for (std::size_t pos = 0; pos + 3 <= t.size(); pos += 3)
            if (t.substr(pos, 3) == "汉") ++han_count;
    }
    EXPECT_EQ(han_count, 400u);   // 没有码点被吞掉
}

TEST(Chunker, HeadingBreadcrumbCarriesPathAndHeadings) {
    const std::string body =
        "# 安装\n"
        "## Linux\n"
        "run the setup script\n"
        "\n"
        "## macOS\n"
        "brew install everything\n";
    const auto chunks = rag::chunk_document("guide.md", body,
                                            /*max_lines=*/3, /*max_chars=*/200,
                                            /*overlap=*/0);
    ASSERT_FALSE(chunks.empty());
    bool found_linux = false, found_macos = false;
    for (const auto& c : chunks) {
        if (c.text.find("setup script") != std::string::npos) {
            EXPECT_EQ(c.context, "guide.md › 安装 › Linux");
            found_linux = true;
        }
        if (c.text.find("brew install") != std::string::npos) {
            EXPECT_EQ(c.context, "guide.md › 安装 › macOS");
            found_macos = true;
        }
    }
    EXPECT_TRUE(found_linux);
    EXPECT_TRUE(found_macos);
}

TEST(Chunker, SkipsWhitespaceOnlyChunks) {
    EXPECT_TRUE(rag::chunk_document("blank.md", "\n\n   \n\t\n",
                                    40, 1600, 4).empty());
}

TEST(Chunker, OverlapRepeatsBoundaryLines) {
    const std::string body = "l1\nl2\nl3\nl4\nl5\nl6";
    const auto chunks = rag::chunk_document("o.md", body,
                                            /*max_lines=*/3, /*max_chars=*/1000,
                                            /*overlap_lines=*/1);
    ASSERT_GE(chunks.size(), 2u);
    // 相邻块之间有行重叠：后块起点 ≤ 前块终点。
    for (std::size_t i = 1; i < chunks.size(); ++i)
        EXPECT_LE(chunks[i].line_start, chunks[i - 1].line_end);
    // overlap 也不能丢内容：每行仍在某个块里。
    const auto lines = split_lines(body);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        bool found = false;
        for (const auto& c : chunks)
            if (c.line_start <= static_cast<int>(i) + 1
                && static_cast<int>(i) + 1 <= c.line_end) { found = true; break; }
        EXPECT_TRUE(found) << "line " << (i + 1) << " lost";
    }
}

// 最多 3 空格缩进的 ``` 仍是合法围栏（CommonMark）。不认缩进围栏时，
// fence 状态从未进入，围栏内的空行会被当成语义断点，代码块被劈开。
TEST(Chunker, IndentedFenceKeepsBlockIntact) {
    const std::string body =
        "intro line\n"
        "  ```bash\n"
        "a=1\n"
        "\n"
        "b=2\n"
        "```\n"
        "after fence\n";
    const auto chunks = rag::chunk_document("f.md", body, 40, 1000, 0);
    const rag::Chunk* code = nullptr;
    for (const auto& c : chunks)
        if (c.text.find("a=1") != std::string::npos) code = &c;
    ASSERT_NE(code, nullptr);
    EXPECT_NE(code->text.find("b=2"), std::string::npos);
}

// ~~~ 是等价的围栏标记（CommonMark）。同样要进 fence 状态。
TEST(Chunker, TildeFenceKeepsBlockIntact) {
    const std::string body =
        "intro line\n"
        "~~~python\n"
        "x = 1\n"
        "\n"
        "y = 2\n"
        "~~~\n"
        "after fence\n";
    const auto chunks = rag::chunk_document("t.md", body, 40, 1000, 0);
    const rag::Chunk* code = nullptr;
    for (const auto& c : chunks)
        if (c.text.find("x = 1") != std::string::npos) code = &c;
    ASSERT_NE(code, nullptr);
    EXPECT_NE(code->text.find("y = 2"), std::string::npos);
}

// ── C. BM25：建索引 + 打分检索契约 ────────────────────────────────────────

TEST(Bm25, RareTermRanksItsChunkFirst) {
    const std::vector<rag::Chunk> chunks{
        make_chunk("the quick brown fox jumps"),
        make_chunk("lazy dogs sleep all afternoon"),
        make_chunk("pelican migration over oceans"),
        make_chunk("compiler passes and inlining"),
    };
    const auto idx = rag::build_bm25(chunks);
    EXPECT_EQ(idx.doc_count, 4u);
    EXPECT_EQ(idx.doc_len, (std::vector<std::uint32_t>{5, 5, 4, 4}));
    EXPECT_DOUBLE_EQ(idx.avg_doc_len, 4.5);

    const auto hits = rag::bm25_search(idx, "pelican", 4);
    ASSERT_FALSE(hits.empty());
    EXPECT_EQ(hits[0].first, 2u);

    const auto hits2 = rag::bm25_search(idx, "compiler inlining", 4);
    ASSERT_FALSE(hits2.empty());
    EXPECT_EQ(hits2[0].first, 3u);
}

TEST(Bm25, MultiWordCoverageBeatsSingleWordRepetition) {
    const std::vector<rag::Chunk> chunks{
        make_chunk("chunker soft limit hard limit"),   // 覆盖两个查询词
        make_chunk("chunker chunker overview"),        // 只中一个词，tf=2
        make_chunk("unrelated content entirely"),
    };
    const auto idx = rag::build_bm25(chunks);
    const auto hits = rag::bm25_search(idx, "chunker limit", 3);
    ASSERT_GE(hits.size(), 2u);
    EXPECT_EQ(hits[0].first, 0u);
    EXPECT_GT(hits[0].second, hits[1].second);   // 明确胜出，非侥幸
}

TEST(Bm25, RareTermOutranksUbiquitousTerm) {
    const std::vector<rag::Chunk> chunks{
        make_chunk("deploy config and common setup"),
        make_chunk("logging common severity levels"),
        make_chunk("pelican migration common over oceans"),
    };
    const auto idx = rag::build_bm25(chunks);
    // "common" 在全部 3 个文档出现（idf≈0.13）；"pelican" 只在 1 个（idf≈1.2）。
    const auto hits = rag::bm25_search(idx, "common pelican", 3);
    ASSERT_GE(hits.size(), 3u);
    EXPECT_EQ(hits[0].first, 2u);
}

TEST(Bm25, SaturationCapsTermFrequencyAtK1) {
    // 等长文档（dl=12, avgdl=12 → 长度项=1），同一查询词 tf=1 vs tf=10。
    // 期望值由公式手算（独立真值，非从实现反推）：
    //   idf     = ln((2-2+0.5)/(2+0.5) + 1) = ln(1.2)
    //   sat(1)  = 1·(1+1.5)/(1 + 1.5·1)     = 1.0
    //   sat(10) = 10·(1+1.5)/(10 + 1.5·1)   = 25/11.5 ≈ 2.174
    const std::vector<rag::Chunk> chunks{
        make_chunk("kangaroo pad pad pad pad pad pad pad pad pad pad pad"),
        make_chunk("kangaroo kangaroo kangaroo kangaroo kangaroo kangaroo "
                   "kangaroo kangaroo kangaroo kangaroo pad pad"),
    };
    const auto idx = rag::build_bm25(chunks);
    const auto hits = rag::bm25_search(idx, "kangaroo", 2);
    ASSERT_EQ(hits.size(), 2u);
    double s1 = 0.0, s10 = 0.0;
    for (const auto& [doc, score] : hits) {
        if (doc == 0u) s1 = score;
        else           s10 = score;
    }
    const double idf = std::log(0.5 / 2.5 + 1.0);
    EXPECT_NEAR(s1,  idf * 1.0,        1e-9);
    EXPECT_NEAR(s10, idf * 25.0 / 11.5, 1e-9);
    EXPECT_LT(s10, 10.0 * s1);   // tf×10 换不来 10×分数 —— 饱和封顶
}

TEST(Bm25, ShortDocOutranksLongDocAtEqualTf) {
    std::string long_doc = "zebra";
    for (int i = 0; i < 30; ++i) long_doc += " fill";   // dl=31
    const std::vector<rag::Chunk> chunks{
        make_chunk("zebra facts"),      // dl=2，同样的 tf=1
        make_chunk(std::move(long_doc)),
    };
    const auto idx = rag::build_bm25(chunks);
    const auto hits = rag::bm25_search(idx, "zebra", 2);
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0].first, 0u);
}

TEST(Bm25, NoMatchReturnsEmptyAndTopKTruncates) {
    const std::vector<rag::Chunk> chunks{
        make_chunk("alpha bravo one"),
        make_chunk("alpha bravo two"),
        make_chunk("alpha bravo three"),
    };
    const auto idx = rag::build_bm25(chunks);
    EXPECT_TRUE(rag::bm25_search(idx, "nonexistent term", 5).empty());
    const auto hits = rag::bm25_search(idx, "alpha bravo", 2);
    EXPECT_EQ(hits.size(), 2u);   // 3 个块都命中，但只取前 k
}

TEST(Bm25, EqualScoresTieBreakByChunkId) {
    const std::vector<rag::Chunk> chunks{
        make_chunk("alpha xxx"),
        make_chunk("alpha yyy"),
        make_chunk("alpha zzz"),
    };
    const auto idx = rag::build_bm25(chunks);
    const auto hits = rag::bm25_search(idx, "alpha", 3);
    ASSERT_EQ(hits.size(), 3u);
    EXPECT_EQ(hits[0].second, hits[1].second);
    EXPECT_EQ(hits[1].second, hits[2].second);
    EXPECT_EQ(hits[0].first, 0u);
    EXPECT_EQ(hits[1].first, 1u);
    EXPECT_EQ(hits[2].first, 2u);
}

TEST(Bm25, BreadcrumbTermsMakeBodylessChunkFindable) {
    const std::string body =
        "# 安装\n"
        "## Linux\n"
        "\n"
        "run the setup script\n"
        "\n"
        "## macOS\n"
        "\n"
        "brew install everything\n";
    const auto all = rag::chunk_document("guide.md", body, 3, 200, 0);
    // 只保留正文里没有 "linux" 的块 —— 让「面包屑里有 linux」成为唯一线索。
    std::vector<rag::Chunk> chunks;
    for (const auto& c : all)
        if (c.text.find("inux") == std::string::npos) chunks.push_back(c);
    const auto idx = rag::build_bm25(chunks);
    const auto hits = rag::bm25_search(idx, "linux", 5);
    ASSERT_FALSE(hits.empty());
    EXPECT_NE(chunks[hits[0].first].text.find("setup script"),
              std::string::npos);
}

TEST(Bm25, ChineseCorpusIsSearchableViaBigrams) {
    const std::vector<rag::Chunk> chunks{
        make_chunk("注册表的配置方法"),
        make_chunk("日志系统的初始化"),
        make_chunk("网络连接的超时设置"),
    };
    const auto idx = rag::build_bm25(chunks);
    const auto hits = rag::bm25_search(idx, "如何配置注册表", 3);
    // ↑ agentty 原版 tokenizer 在这里直接失明（中文产出零 token）。
    ASSERT_FALSE(hits.empty());
    EXPECT_EQ(hits.size(), 1u);   // 只有块 0 与查询共享 bigram
    EXPECT_EQ(hits[0].first, 0u);
}

TEST(EmbedInput,NomicDocAndQueryPrefixes) {
    EXPECT_EQ(rag::embed_input_text("nomic-embed-text", rag::EmbedRole::Doc, "安装指南"),
            "search_document: 安装指南");
    EXPECT_EQ(rag::embed_input_text("nomic-embed-text", rag::EmbedRole::Query,"怎么安装"),
              "search_query: 怎么安装");
}

TEST(EmbedInput, ModelMatchIsCaseInsensitiveSubstring) {
    // 教材同款：小写化后子串匹配 —— 带标签的 "nomic-embed-text:latest" 也要命中
    EXPECT_EQ(rag::embed_input_text("Nomic-Embed-Text:latest",rag::EmbedRole::Doc, "x"),
              "search_document: x");
}

TEST(EmbedInput, E5FamilyUsesPassageAndQuery) {
    EXPECT_EQ(rag::embed_input_text("multilingual-e5-large",rag::EmbedRole::Doc, "x"),
              "passage: x");
    EXPECT_EQ(rag::embed_input_text("multilingual-e5-large",rag::EmbedRole::Query, "x"),
              "query: x");
}

TEST(EmbedInput, UnknownModelGetsNoPrefix) {
    // 错误前缀会伤害不认识它的模型 —— 认不出的名字必须原样放行
    EXPECT_EQ(rag::embed_input_text("llama3", rag::EmbedRole::Doc, "原文"),
              "原文");
    EXPECT_EQ(rag::embed_input_text("llama3", rag::EmbedRole::Query, "原文"),
              "原文");
}

TEST(EmbedRequest, CarriesModelAndOrderedInputArray) {
    const auto body = rag::build_embed_request_body(
        "nomic-embed-text", {"第一条", "second text"});
    const auto j = nlohmann::json::parse(body);   // 能严格解析 = 是合法 JSON
    EXPECT_EQ(j.at("model"), "nomic-embed-text");
    ASSERT_EQ(j.at("input").size(), 2u);
    EXPECT_EQ(j.at("input")[0], "第一条");
    EXPECT_EQ(j.at("input")[1], "second text");
}

TEST(EmbedRequest, EscapesJsonSpecialsInText) {
    // 文本带引号/制表/换行也必须活着过线：靠严格 JSON 转义，不是字符串拼接
    const auto body =
        rag::build_embed_request_body("m", {"quote\" and\ttab\nnewline"});
    const auto j = nlohmann::json::parse(body);
    EXPECT_EQ(j.at("input")[0].get<std::string>(),
              "quote\" and\ttab\nnewline");
}

TEST(EmbedResponse, HappyPathReturnsAlignedVectors) {
    const auto r = rag::parse_embed_response(
        R"({"embeddings":[[0.1,0.2,0.3],[0.4,0.5,0.6]]})", 2);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 2u);
    ASSERT_EQ((*r)[0].size(), 3u);
    EXPECT_FLOAT_EQ((*r)[0][0], 0.1f);
    EXPECT_FLOAT_EQ((*r)[1][2], 0.6f);
}

TEST(EmbedResponse, LegacySingleShapeOnlyValidForCountOne) {
    // 旧端点的单数 "embedding"：单条请求时合法（教材兼容同款）
    const auto ok =
        rag::parse_embed_response(R"({"embedding":[0.5,0.6]})", 1);
    ASSERT_TRUE(ok.has_value());
    EXPECT_FLOAT_EQ((*ok)[0][1], 0.6f);
    // 但批量请求拿到它 = 数量失守 → err
    const auto bad =
        rag::parse_embed_response(R"({"embedding":[0.5,0.6]})", 2);
    EXPECT_FALSE(bad.has_value());
}

TEST(EmbedResponse, CountMismatchIsError) {
    // 数量守卫是解析层最要紧的一道岗：
    // 错位（chunk-id↔向量对不上）比整批失败危险得多 —— 失败会降级，错位是静默腐烂
    EXPECT_FALSE(rag::parse_embed_response(
        R"({"embeddings":[[0.1],[0.2]]})", 3).has_value());
}

TEST(EmbedResponse, RaggedRowsAreError) {
    // 行长不齐 = 响应损坏（768 维模型不会返回 767 维）
    EXPECT_FALSE(rag::parse_embed_response(
        R"({"embeddings":[[0.1,0.2],[0.3]]})", 2).has_value());
}

TEST(EmbedResponse, MalformedBodiesAreErrorNotCrash) {
    EXPECT_FALSE(rag::parse_embed_response(R"({"foo":1})", 1).has_value());            // 缺字段
    EXPECT_FALSE(rag::parse_embed_response(R"({"embeddings":"x"})", 1).has_value());   // 顶层非数组
    EXPECT_FALSE(rag::parse_embed_response(R"({"embeddings":[0.1]})", 1).has_value()); // 行非数组
    EXPECT_FALSE(rag::parse_embed_response(R"({"embeddings":[["a"]]})", 1).has_value()); // 非数字元素
    EXPECT_FALSE(rag::parse_embed_response(R"({"embeddings":[[]]})", 1).has_value());  // 空行
    EXPECT_FALSE(rag::parse_embed_response(R"({"embeddings":[]})", 1).has_value());    // 空批
    EXPECT_FALSE(rag::parse_embed_response("not json at all", 1).has_value());         // 垃圾体
}

TEST(Cosine, IdenticalVectorsScoreOne) {
    const std::vector<float> v{0.3f, -0.4f, 0.5f};
    EXPECT_NEAR(rag::cosine_sim(v, v), 1.0, 1e-9);
}

TEST(Cosine, OrthogonalScoresZeroAndOppositeScoresMinusOne) {
    EXPECT_NEAR(rag::cosine_sim({1.f, 0.f}, {0.f, 1.f}), 0.0, 1e-9);
    EXPECT_NEAR(rag::cosine_sim({1.f, 0.f}, {-1.f, 0.f}), -1.0, 1e-9);
}

TEST(Cosine, ScaleInvariantBecauseDirectionIsMeaning) {
    // 模长是干扰项（文本长度等 nuisance），只有方向编码意思 ——
    // 这就是「cosine 不用欧氏距离」的全部理由，写进测试钉死
    const std::vector<float> a{0.1f, 0.2f};
    const std::vector<float> b{1.f, 2.f};   // 同方向、10 倍模长
    EXPECT_NEAR(rag::cosine_sim(a, b), 1.0, 1e-9);
}

TEST(Cosine, MismatchedDimsAndEmptyScoreZero) {
    // 维度不齐 = 状态已腐（建库和查询用了不同维度的模型）：
    // 返回 0 = 零相似、退出竞争 —— 宁可少一个候选，绝不拿错位向量算分
    EXPECT_EQ(rag::cosine_sim({1.f, 2.f}, {1.f, 2.f, 3.f}), 0.0);
    EXPECT_EQ(rag::cosine_sim({}, {}), 0.0);
}

TEST(RankedIds, StripsScoresKeepsOrder) {
    const std::vector<std::pair<std::uint32_t, double>> scored{
        {7u, 9.9}, {2u, 3.3}, {5u, 0.1}};
    EXPECT_EQ(rag::ranked_ids(scored),
              (std::vector<std::uint32_t>{7u, 2u, 5u}));
    EXPECT_TRUE(rag::ranked_ids({}).empty());
}

TEST(Rrf, SingleListKeepsItsRankOrder) {
    // 融合分 = 1/(k + rank_1based)（rank 从 1 数起 —— 教材同款 off-by-one 语义）
    const auto fused = rag::rrf_fuse({{5u, 3u, 1u}}, 60.0, 2);
    ASSERT_EQ(fused.size(), 2u);
    EXPECT_EQ(fused[0].first, 5u);
    EXPECT_DOUBLE_EQ(fused[0].second, 1.0 / 61.0);
    EXPECT_EQ(fused[1].first, 3u);
    EXPECT_DOUBLE_EQ(fused[1].second, 1.0 / 62.0);
}

TEST(Rrf, BothRoadsAgreeBeatsOneRoad) {
    // 两路共识 > 单路意见：2 号在两路各拿一个位次，压过只在一路拿第一的 1 号
    // —— 这是「为什么融合」的最小实例
    const auto fused = rag::rrf_fuse({{1u, 2u}, {2u, 3u}}, 60.0, 3);
    ASSERT_EQ(fused.size(), 3u);
    EXPECT_EQ(fused[0].first, 2u);   // 1/61 + 1/62
    EXPECT_EQ(fused[1].first, 1u);   // 1/61
    EXPECT_EQ(fused[2].first, 3u);   // 1/62
    EXPECT_DOUBLE_EQ(fused[0].second, 1.0 / 61.0 + 1.0 / 62.0);
}

TEST(Rrf, EqualFusedScoresTieBreakByChunkId) {
    const auto fused = rag::rrf_fuse({{9u}, {2u}}, 60.0, 2);
    ASSERT_EQ(fused.size(), 2u);
    EXPECT_EQ(fused[0].first, 2u);   // 同为 1/61 → 确定性：id 升序
    EXPECT_EQ(fused[1].first, 9u);
}

TEST(Rrf, TruncatesToOutK) {
    const auto fused = rag::rrf_fuse({{4u, 1u, 7u}}, 60.0, 1);
    ASSERT_EQ(fused.size(), 1u);
    EXPECT_EQ(fused[0].first, 4u);
}

TEST(Rrf, DeepRankStillContributes) {
    // 候选池深度的存在理由：第 40 名不是零分，是 1/(60+40)——
    // 深位候选仍能在两路叠加后浮上来（hybrid_search 的 pool 语义由此而来）
    std::vector<std::uint32_t> list(40);
    for (std::uint32_t i = 0; i < 40; ++i) list[i] = i;
    const auto fused = rag::rrf_fuse({list}, 60.0, 40);
    ASSERT_EQ(fused.size(), 40u);
    EXPECT_EQ(fused[39].first, 39u);
    EXPECT_DOUBLE_EQ(fused[39].second, 1.0 / 100.0);
}

TEST(Rrf, EmptyInputGivesEmptyOutput) {
    EXPECT_TRUE(rag::rrf_fuse({}, 60.0, 5).empty());
    EXPECT_TRUE(rag::rrf_fuse({{}, {}}, 60.0, 5).empty());
}

// ── 切片 #41 Red 第三批：建库与混合查询（假 backend 模拟模型）────────────
// EmbedBackend 是 std::function —— 测试里用 lambda 假扮 Ollama：
// 按文本内容分发可控向量、记录调用形状（批量数/角色）、按需注入故障。
// 这就是「可注入 backend」决策的全部回报：不碰网络就能钉死管线语义。

TEST(EmbedCorpus, BatchesAt64WithDocRoleAndMapsById) {
    std::vector<rag::Chunk> chunks;
    for (int i = 0; i < 130; ++i)
        chunks.push_back(make_chunk("c" + std::to_string(i)));

    std::vector<std::size_t> batch_sizes;
    std::vector<rag::EmbedRole> roles;
    const rag::EmbedBackend backend =
        [&](const rag::EmbedConfig&,
            const std::vector<std::string>& texts,
            rag::EmbedRole role)
            -> std::expected<std::vector<std::vector<float>>, std::string> {
            batch_sizes.push_back(texts.size());
            roles.push_back(role);
            std::vector<std::vector<float>> out;
            for (const auto& t : texts)
                out.push_back({static_cast<float>(std::stoi(t.substr(1))),
                               1.f});
            return out;
        };

    const auto dense = rag::embed_corpus(chunks, backend, rag::EmbedConfig{});
    ASSERT_TRUE(dense.has_value());
    EXPECT_EQ(dense->vecs.size(), 130u);
    ASSERT_EQ(dense->vecs[42].size(), 2u);
    EXPECT_FLOAT_EQ(dense->vecs[42][0], 42.f);   // id↔向量对位：第 42 块拿第 42 个向量
    ASSERT_EQ(batch_sizes.size(), 3u);           // 130 = 64 + 64 + 2
    EXPECT_EQ(batch_sizes[0], 64u);
    EXPECT_EQ(batch_sizes[1], 64u);
    EXPECT_EQ(batch_sizes[2], 2u);
    for (rag::EmbedRole r : roles) EXPECT_EQ(r, rag::EmbedRole::Doc);
}

TEST(EmbedCorpus, EmbedInputIsContextPrefixedBody) {
    rag::Chunk c = make_chunk("运行安装脚本");
    c.context = "guide.md › 安装 › Linux";
    std::string seen;
    const rag::EmbedBackend backend =
        [&](const rag::EmbedConfig&,
            const std::vector<std::string>& texts,
            rag::EmbedRole)
            -> std::expected<std::vector<std::vector<float>>, std::string> {
            seen = texts.front();
            return std::vector<std::vector<float>>{{1.f}};
        };
    ASSERT_TRUE(
        rag::embed_corpus({c}, backend, rag::EmbedConfig{}).has_value());
    // 面包屑前缀（context 空则裸 text —— 上一测试的 "c42" 已钉住该分支）
    EXPECT_EQ(seen, "guide.md › 安装 › Linux\n运行安装脚本");
}

TEST(EmbedCorpus, AnyBatchFailureFailsWholeCorpus) {
    std::vector<rag::Chunk> chunks;
    for (int i = 0; i < 65; ++i)
        chunks.push_back(make_chunk("c" + std::to_string(i)));
    std::size_t calls = 0;
    const rag::EmbedBackend backend =
        [&](const rag::EmbedConfig&,
            const std::vector<std::string>& texts,
            rag::EmbedRole)
            -> std::expected<std::vector<std::vector<float>>, std::string> {
            if (++calls == 2)   // 第二批（第 65 块那批）失败
                return std::unexpected(std::string{"模拟后端故障"});
            return std::vector<std::vector<float>>(texts.size(), {1.f});
        };
    // 整体 err —— 绝不交出 64/65 的半截 dense（语料中段的静默质量悬崖）
    EXPECT_FALSE(
        rag::embed_corpus(chunks, backend, rag::EmbedConfig{}).has_value());
    EXPECT_EQ(calls, 2u);
}

TEST(EmbedCorpus, RaggedDimsAcrossBatchesIsError) {
    std::vector<rag::Chunk> chunks;
    for (int i = 0; i < 70; ++i)
        chunks.push_back(make_chunk("c" + std::to_string(i)));
    std::size_t calls = 0;
    const rag::EmbedBackend backend =
        [&](const rag::EmbedConfig&,
            const std::vector<std::string>& texts,
            rag::EmbedRole)
            -> std::expected<std::vector<std::vector<float>>, std::string> {
            const std::size_t dim = (++calls == 1) ? 2 : 3;   // 跨批换维度
            return std::vector<std::vector<float>>(
                texts.size(), std::vector<float>(dim, 1.f));
        };
    EXPECT_FALSE(
        rag::embed_corpus(chunks, backend, rag::EmbedConfig{}).has_value());
    // 两批都被调过 + 这个假 backend 从不主动失败 → err 只能来自维度守卫。
    // （没有这条断言，本测试会被「无条件 err」的桩假绿 —— Red 阶段的陷阱）
    EXPECT_EQ(calls, 2u);
}

TEST(EmbedCorpus, EmptyCorpusGivesEmptyDenseNoCalls) {
    bool called = false;
    const rag::EmbedBackend backend =
        [&](const rag::EmbedConfig&,
            const std::vector<std::string>&,
            rag::EmbedRole)
            -> std::expected<std::vector<std::vector<float>>, std::string> {
            called = true;
            return std::vector<std::vector<float>>{};
        };
    const auto dense = rag::embed_corpus({}, backend, rag::EmbedConfig{});
    ASSERT_TRUE(dense.has_value());       // 空 dense 是合法状态（BM25-only）
    EXPECT_TRUE(dense->vecs.empty());
    EXPECT_FALSE(called);
}

TEST(Hybrid, FusesBm25AndDenseIntoRRFOrder) {
    // 词法路（query "beta"）：A(=0) 第1、B(=1) 第2，C(=2) 零交集缺席
    // 语义路（假向量）：dense 名次 B, C, A（全为正相似）
    const std::vector<rag::Chunk> chunks{
        make_chunk("beta"),         // A: id 0
        make_chunk("alpha beta"),   // B: id 1
        make_chunk("alpha alpha"),  // C: id 2
    };
    const rag::Bm25Index idx = rag::build_bm25(chunks);
    const rag::DenseIndex dense{{{0.f, 1.f}, {1.f, 0.f}, {1.f, 1.f}}};
    const rag::EmbedConfig cfg{.model = "fake-model"};
    const rag::EmbedBackend backend =
        [](const rag::EmbedConfig&,
           const std::vector<std::string>& texts,
           rag::EmbedRole role)
           -> std::expected<std::vector<std::vector<float>>, std::string> {
        EXPECT_EQ(texts.size(), 1u);               // 查询只向量化一次
        EXPECT_EQ(role, rag::EmbedRole::Query);
        return std::vector<std::vector<float>>{{1.f, 0.1f}};
    };
    const auto fused = rag::hybrid_search(idx, dense, "beta", backend, cfg, 3);
    ASSERT_EQ(fused.hits.size(), 3u);
    // RRF 手算：B = 1/62(词法r2) + 1/61(语义r1) > A = 1/61 + 1/63 > C = 1/62
    EXPECT_EQ(fused.hits[0].first, 1u);
    EXPECT_EQ(fused.hits[1].first, 0u);
    EXPECT_EQ(fused.hits[2].first, 2u);   // C 词法路根本没看见 —— 靠语义路浮上来
    EXPECT_DOUBLE_EQ(fused.hits[0].second, 1.0 / 62.0 + 1.0 / 61.0);
}

TEST(Hybrid, ZeroCosineChunksStayOutOfDenseRank) {
    // 零相似（无信号/维度腐烂）不得进 dense 名次表 ——
    // 否则 RRF 白送它 1/(60+rank)，纯噪音
    const std::vector<rag::Chunk> chunks{
        make_chunk("beta"),         // A: id 0
        make_chunk("gamma gamma"),  // Z: id 1
    };
    const rag::Bm25Index idx = rag::build_bm25(chunks);
    const rag::DenseIndex dense{{{1.f, 0.f}, {0.f, 1.f}}};
    const rag::EmbedConfig cfg{.model = "fake-model"};
    const rag::EmbedBackend backend =
        [](const rag::EmbedConfig&,
           const std::vector<std::string>&,
           rag::EmbedRole)
           -> std::expected<std::vector<std::vector<float>>, std::string> {
        return std::vector<std::vector<float>>{{1.f, 0.f}};
    };
    const auto fused = rag::hybrid_search(idx, dense, "beta", backend, cfg, 5);
    ASSERT_EQ(fused.hits.size(), 1u);   // Z 两路都没份
    EXPECT_EQ(fused.hits[0].first, 0u);
    // A = 词法第1名 + 语义第1名
    EXPECT_DOUBLE_EQ(fused.hits[0].second, 2.0 / 61.0);
}

TEST(Hybrid, EmptyDenseDegradesToBm25WithoutCallingBackend) {
    const std::vector<rag::Chunk> chunks{make_chunk("beta"),
                                         make_chunk("alpha beta")};
    const rag::Bm25Index idx = rag::build_bm25(chunks);
    bool called = false;
    const rag::EmbedBackend backend =
        [&](const rag::EmbedConfig&,
            const std::vector<std::string>&,
            rag::EmbedRole)
            -> std::expected<std::vector<std::vector<float>>, std::string> {
        called = true;
        return std::vector<std::vector<float>>{{1.f}};
    };
    const rag::EmbedConfig cfg{.model = "fake-model"};
    const auto fused =
        rag::hybrid_search(idx, rag::DenseIndex{}, "beta", backend, cfg, 2);
    EXPECT_EQ(fused.hits, rag::bm25_search(idx, "beta", 2));   // 逐位一致，含原始分
    EXPECT_FALSE(called);   // dense 空 → 查询向量化都是浪费
}

TEST(Hybrid, QueryEmbedFailureDegradesToBm25) {
    const std::vector<rag::Chunk> chunks{make_chunk("beta"),
                                         make_chunk("alpha beta")};
    const rag::Bm25Index idx = rag::build_bm25(chunks);
    const rag::DenseIndex dense{{{1.f, 0.f}, {0.f, 1.f}}};
    const rag::EmbedBackend backend =
        [](const rag::EmbedConfig&,
           const std::vector<std::string>&,
           rag::EmbedRole)
           -> std::expected<std::vector<std::vector<float>>, std::string> {
        return std::unexpected(std::string{"模拟查询向量化失败"});
    };
    const rag::EmbedConfig cfg{.model = "fake-model"};
    const auto fused =
        rag::hybrid_search(idx, dense, "beta", backend, cfg, 2);
    EXPECT_EQ(fused.hits, rag::bm25_search(idx, "beta", 2));
}

TEST(Hybrid, EmptyModelDegradesWithoutCallingBackend) {
    const std::vector<rag::Chunk> chunks{make_chunk("beta"),
                                         make_chunk("alpha beta")};
    const rag::Bm25Index idx = rag::build_bm25(chunks);
    bool called = false;
    const rag::EmbedBackend backend =
        [&](const rag::EmbedConfig&,
           const std::vector<std::string>&,
           rag::EmbedRole)
            -> std::expected<std::vector<std::vector<float>>, std::string> {
        called = true;
        return std::vector<std::vector<float>>{{1.f}};
    };
    const rag::EmbedConfig cfg{.model = ""};   // 未配模型 = BM25-only
    const auto fused =
        rag::hybrid_search(idx, rag::DenseIndex{}, "beta", backend, cfg, 2);
    EXPECT_EQ(fused.hits, rag::bm25_search(idx, "beta", 2));
    EXPECT_FALSE(called);
}
// ── #43 接缝扩展：SearchMode（降级真相从降级发生处带出）──────────────────
// 模式是量纲声明：hybrid = RRF 融合分，BM25-only = BM25 原始分。
// search_docs 工具的 mode 行直接抄这里的结果，猜错 = 对模型撒谎。

TEST(SearchMode, HybridReportsHybridWhenBothPathsLive) {
    const std::vector<rag::Chunk> chunks{
        make_chunk("beta"),         // A: id 0
        make_chunk("alpha beta"),   // B: id 1
        make_chunk("gamma gamma"),  // C: id 2
    };
    const rag::Bm25Index idx = rag::build_bm25(chunks);
    const rag::DenseIndex dense{{{0.f, 1.f}, {1.f, 0.f}, {1.f, 1.f}}};
    const rag::EmbedConfig cfg{.model = "fake-model"};
    const rag::EmbedBackend backend =
        [](const rag::EmbedConfig&,
           const std::vector<std::string>&,
           rag::EmbedRole)
           -> std::expected<std::vector<std::vector<float>>, std::string> {
        return std::vector<std::vector<float>>{{1.f, 0.1f}};
    };
    const auto r = rag::hybrid_search(idx, dense, "beta", backend, cfg, 3);
    EXPECT_EQ(r.mode, rag::SearchMode::Hybrid);
}

TEST(SearchMode, DegradePathsAllReportBm25Only) {
    const std::vector<rag::Chunk> chunks{make_chunk("beta"),
                                         make_chunk("alpha beta")};
    const rag::Bm25Index idx = rag::build_bm25(chunks);
    const rag::DenseIndex dense{{{1.f, 0.f}, {0.f, 1.f}}};
    const rag::EmbedConfig cfg{.model = "fake-model"};

    // ① dense 空
    const rag::EmbedBackend ok_backend =
        [](const rag::EmbedConfig&,
           const std::vector<std::string>&,
           rag::EmbedRole)
           -> std::expected<std::vector<std::vector<float>>, std::string> {
        return std::vector<std::vector<float>>{{1.f, 0.f}};
    };
    EXPECT_EQ(rag::hybrid_search(idx, rag::DenseIndex{}, "beta", ok_backend,
                                 cfg, 2).mode,
              rag::SearchMode::Bm25Only);

    // ② model 空（未配置 = 合法降级，不是错误）
    EXPECT_EQ(rag::hybrid_search(idx, dense, "beta", ok_backend,
                                 rag::EmbedConfig{}, 2).mode,
              rag::SearchMode::Bm25Only);

    // ③ query embed 失败（瞬断场景 —— 事后猜模式在这里撒谎）
    const rag::EmbedBackend fail_backend =
        [](const rag::EmbedConfig&,
           const std::vector<std::string>&,
           rag::EmbedRole)
           -> std::expected<std::vector<std::vector<float>>, std::string> {
        return std::unexpected(std::string{"模拟瞬断"});
    };
    EXPECT_EQ(rag::hybrid_search(idx, dense, "beta", fail_backend, cfg, 2).mode,
              rag::SearchMode::Bm25Only);

    // ④ dense 名次表为空（全库零相似/无正相似）
    //const rag::DenseIndex rotten{{{2.f, 0.f}, {0.f, 2.f}}};  // 与查询向量正交  (错误。为同向而非正交)
    const rag::DenseIndex rotten{{{0.f, 1.f}, {0.f, 2.f}}};
    EXPECT_EQ(rag::hybrid_search(idx, rotten, "beta", ok_backend, cfg, 2).mode,
              rag::SearchMode::Bm25Only);

    // 只有有任一一个结果与查询不是零相似，那么 embedding 就会参与评分，此时会进入 Hybird
    const rag::DenseIndex rotten1{{{2.f, 0.f}, {0.f, 2.f}}};
    EXPECT_EQ(rag::hybrid_search(idx, rotten1, "beta", ok_backend, cfg, 2).mode,
              rag::SearchMode::Hybrid);
}

TEST(SearchMode, CorpusSearchPropagatesMode) {
    const std::vector<rag::Chunk> chunks{make_chunk("beta")};
    const rag::EmbedBackend backend =
        [](const rag::EmbedConfig&,
           const std::vector<std::string>& texts,
           rag::EmbedRole role)
           -> std::expected<std::vector<std::vector<float>>, std::string> {
        (void)texts;
        (void)role;
        return std::vector<std::vector<float>>(texts.size(),
                                               std::vector<float>{1.f, 0.f});
    };
    rag::Corpus c;
    c.build_from_memory({{"a.md", "beta content"}}, backend,
                        rag::EmbedConfig{.model = "fake-model"});
    ASSERT_TRUE(c.has_embeddings());
    EXPECT_EQ(c.search("beta", backend,
                       rag::EmbedConfig{.model = "fake-model"}, 3).mode,
              rag::SearchMode::Hybrid);
    EXPECT_EQ(c.search("beta", backend, rag::EmbedConfig{}, 3).mode,
              rag::SearchMode::Bm25Only);
}

// ── 切片 #42：Corpus 目录建库 + 增量缓存 ─────────────────────────────────
// 夹具约定：三个主题词互斥的文档（zephyr∈A / harbor∈B / quartz∈C），
// 检索断言才有唯一答案；假后端向量按文本内容确定性生成，同文本永远同
// 向量 —— 缓存命中前后的检索结果才可比。块身份比较一律用 (path, 行区间)，
// 不比 chunk-id：目录遍历顺序不保证跨进程稳定。

namespace {

namespace fs = std::filesystem;

// RAII 临时目录：测试语料的家，析构时连缓存一起清掉。
class TempDir {
public:
    TempDir() {
        static std::uint64_t seq = 0;
        const auto ns =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / ("my_agent_rag_test_"
                                             + std::to_string(ns) + "_"
                                             + std::to_string(seq++));
        fs::create_directories(path_);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path_, ec); }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void write_file(const fs::path& p, std::string_view content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << std::string(content);
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

// 可编程假后端：记录 Doc 角色收到的输入、调用计数、可注入失败。
struct FakeBackend {
    std::vector<std::string> doc_inputs;
    std::size_t calls = 0;
    bool fail = false;

    [[nodiscard]] rag::EmbedBackend fn() {
        return [this](const rag::EmbedConfig&,
                      const std::vector<std::string>& texts,
                      rag::EmbedRole role)
            -> std::expected<std::vector<std::vector<float>>, std::string> {
            ++calls;
            if (fail) return std::unexpected(std::string{"模拟后端故障"});
            if (role == rag::EmbedRole::Doc)
                doc_inputs.insert(doc_inputs.end(), texts.begin(), texts.end());
            // 确定性伪向量：同文本永远同向量（维度恒 2，跨批不乱）
            std::vector<std::vector<float>> out;
            out.reserve(texts.size());
            for (const auto& t : texts)
                out.push_back({static_cast<float>(t.size() % 89 + 1), 1.f});
            return out;
        };
    }
};

// Corpus 喂给 backend 的块输入应与「直接 chunk_document + 拼装」逐条一致
// （context 恒含路径非空，但保留 context 空则裸 text 的分支以防回归）。
std::vector<std::string> expected_doc_inputs(const std::string& path,
                                             const std::string& body) {
    std::vector<std::string> out;
    for (const auto& c : rag::chunk_document(path, body))
        out.push_back(c.context.empty() ? c.text : c.context + "\n" + c.text);
    return out;
}

std::string chunk_key(const rag::Chunk& c) {
    return c.path + ":" + std::to_string(c.line_start) + "-"
         + std::to_string(c.line_end);
}

const char* kDocA =
    "# Alpha Guide\n\nThe installation requires the zephyr package.\n"
    "Run the installer with default settings to finish setup.\n";
const char* kDocB =
    "# Beta Notes\n\nTroubleshooting the harbor service requires patience.\n"
    "Check the harbor logs before restarting anything.\n";
const char* kDocC =
    "# Gamma Log\n\nThe quartz scheduler emits audit events nightly.\n"
    "Rotate the quartz logs weekly to reclaim disk space.\n";

}  // namespace

TEST(EmbedIdentity, ComposesModelDialectAndRecipe) {
    const std::string id = rag::embed_identity("nomic-embed-text");
    // 构成式三段齐全：model、doc 前缀、输入拼法版本
    EXPECT_NE(id.find("nomic-embed-text"), std::string::npos);
    EXPECT_NE(id.find("search_document: "), std::string::npos);  // 方言=函数输出
    EXPECT_NE(id.find(std::string{rag::kEmbedInputRecipe}), std::string::npos);
    // 模型不同 → 身份不同：同维度不同向量空间不可混库（教材 v5 病历）
    EXPECT_NE(rag::embed_identity("bge-m3"), id);
    // 空模型 → 空身份（BM25-only 会话，缓存身份靠穿透语义保护）
    EXPECT_TRUE(rag::embed_identity("").empty());
}

TEST(CorpusBuild, MissingOrEmptyRootGivesEmptyCorpus) {
    FakeBackend be;
    const rag::EmbedConfig cfg{.model = "fake-embed"};

    // 不存在的目录：空 corpus，不报错
    rag::Corpus c;
    const auto stats = c.build(
        fs::temp_directory_path() / "my_agent_rag_no_such_dir_42",
        be.fn(), cfg);
    EXPECT_EQ(stats.files_seen, 0u);
    EXPECT_EQ(c.chunk_count(), 0u);
    EXPECT_FALSE(c.has_embeddings());
    EXPECT_TRUE(c.search("anything", be.fn(), cfg, 3).hits.empty());

    // 空目录同语义
    TempDir dir;
    rag::Corpus c2;
    const auto stats2 = c2.build(dir.path(), be.fn(), cfg);
    EXPECT_EQ(stats2.files_seen, 0u);
    EXPECT_EQ(c2.chunk_count(), 0u);
    EXPECT_TRUE(c2.search("anything", be.fn(), cfg, 3).hits.empty());

    EXPECT_EQ(be.calls, 0u);   // 空语料一次都不嵌
}

TEST(CorpusBuild, BuildsRecursesAndFiltersThenSearches) {
    TempDir dir;
    write_file(dir.path() / "a.md", kDocA);
    write_file(dir.path() / "sub" / "c.md", kDocC);        // 递归子目录要收
    write_file(dir.path() / "notes.txt", kDocB);           // 非 .md 不收
    write_file(dir.path() / ".hidden" / "h.md", kDocB);    // 点目录不进

    FakeBackend be;
    const rag::EmbedConfig cfg{.model = "fake-embed"};
    rag::Corpus c;
    const auto stats = c.build(dir.path(), be.fn(), cfg);

    EXPECT_EQ(stats.files_seen, 2u);           // 只有 a.md 与 sub/c.md
    EXPECT_EQ(stats.files_reused, 0u);         // 首建全量
    EXPECT_EQ(stats.files_rechunked, 2u);
    ASSERT_GT(c.chunk_count(), 0u);
    ASSERT_TRUE(c.has_embeddings());
    EXPECT_EQ(stats.chunks_embedded, c.chunk_count());
    EXPECT_TRUE(stats.dense_ok);

    // 块的 path 一律是相对 root 的路径（缓存按 rel_path 键控的前提）
    for (const auto& ch : c.chunks())
        EXPECT_TRUE(ch.path == "a.md" || ch.path == "sub/c.md")
            << "非相对路径: " << ch.path;

    // 主题词唯一 → 首命中即正确文档
    const auto ha = c.search("zephyr", be.fn(), cfg, 3);
    ASSERT_FALSE(ha.hits.empty());
    EXPECT_EQ(c.chunks()[ha.hits[0].first].path, "a.md");
    const auto hc = c.search("quartz", be.fn(), cfg, 3);
    ASSERT_FALSE(hc.hits.empty());
    EXPECT_EQ(c.chunks()[hc.hits[0].first].path, "sub/c.md");

    // .txt 里的词在词法路上检索不到（未配模型 → 恒等降级 bm25_search，
    // 空 BM25 命中 = 干净的扩展名过滤证据）
    const rag::EmbedConfig bm25_cfg{};
    EXPECT_TRUE(c.search("harbor", be.fn(), bm25_cfg, 3).hits.empty());
}

TEST(CorpusCache, SecondBuildHitsCacheWithZeroEmbeds) {
    TempDir dir;
    write_file(dir.path() / "a.md", kDocA);
    write_file(dir.path() / "b.md", kDocB);

    const rag::EmbedConfig cfg{.model = "fake-embed"};
    FakeBackend be1;
    rag::Corpus first;
    const auto s1 = first.build(dir.path(), be1.fn(), cfg);
    ASSERT_TRUE(s1.dense_ok);
    ASSERT_GT(first.chunk_count(), 0u);
    const auto total = first.chunk_count();
    EXPECT_GT(be1.calls, 0u);
    // 缓存已落盘
    EXPECT_TRUE(fs::exists(dir.path() / std::string{rag::kCorpusCacheName}));

    // 新实例同 root = 模拟进程重启（issue 验收口径）
    FakeBackend be2;
    rag::Corpus second;
    const auto s2 = second.build(dir.path(), be2.fn(), cfg);
    EXPECT_EQ(s2.files_seen, 2u);
    EXPECT_EQ(s2.files_reused, 2u);       // 全命中
    EXPECT_EQ(s2.files_rechunked, 0u);
    EXPECT_EQ(s2.chunks_embedded, 0u);    // 零嵌入
    EXPECT_EQ(be2.calls, 0u);             // backend 一次都没碰
    ASSERT_TRUE(s2.dense_ok);             // 向量来自缓存搬运
    EXPECT_EQ(second.chunk_count(), total);

    // 缓存搬运没腐坏：同查询首命中同一块（按块身份比，不比 id）
    for (const char* q : {"zephyr", "harbor"}) {
        const auto h1 = first.search(q, be1.fn(), cfg, 3);
        const auto h2 = second.search(q, be2.fn(), cfg, 3);
        ASSERT_FALSE(h1.hits.empty());
        ASSERT_FALSE(h2.hits.empty());
        EXPECT_EQ(chunk_key(first.chunks()[h1.hits[0].first]),
                  chunk_key(second.chunks()[h2.hits[0].first]));
    }
}

TEST(CorpusCache, ChangedFileReembedsOnlyThatFile) {
    TempDir dir;
    write_file(dir.path() / "a.md", kDocA);
    write_file(dir.path() / "b.md", kDocB);
    write_file(dir.path() / "c.md", kDocC);

    const rag::EmbedConfig cfg{.model = "fake-embed"};
    FakeBackend be1;
    rag::Corpus first;
    ASSERT_TRUE(first.build(dir.path(), be1.fn(), cfg).dense_ok);

    // 改 b.md：内容与长度都变（size+mtime 双指纹必然失配）
    const std::string new_b =
        "# Beta Notes\n\nTroubleshooting the lighthouse service is easier.\n"
        "Consult the lighthouse manual for the restart procedure.\n";
    write_file(dir.path() / "b.md", new_b);

    FakeBackend be2;
    rag::Corpus second;
    const auto s2 = second.build(dir.path(), be2.fn(), cfg);
    EXPECT_EQ(s2.files_reused, 2u);        // a.md、c.md 原样搬运
    EXPECT_EQ(s2.files_rechunked, 1u);
    ASSERT_TRUE(s2.dense_ok);

    // 只有 b.md 的块进 backend —— 输入与「直接 chunk_document + 拼装」
    // 逐条一致（多重集合比较：批量切分顺序无关紧要，条目必须恰好相等）
    auto expected = expected_doc_inputs("b.md", new_b);
    ASSERT_FALSE(expected.empty());
    std::sort(expected.begin(), expected.end());
    auto seen = be2.doc_inputs;
    std::sort(seen.begin(), seen.end());
    EXPECT_EQ(seen, expected);
    EXPECT_EQ(s2.chunks_embedded, expected.size());

    // 变更后的内容可检索（重建不是空转）
    const auto hits = second.search("lighthouse", be2.fn(), cfg, 3);
    ASSERT_FALSE(hits.hits.empty());
    EXPECT_EQ(second.chunks()[hits.hits[0].first].path, "b.md");
}

TEST(CorpusCache, DeletedFileDropsItsChunks) {
    TempDir dir;
    write_file(dir.path() / "a.md", kDocA);
    write_file(dir.path() / "b.md", kDocB);
    const rag::EmbedConfig cfg{.model = "fake-embed"};
    FakeBackend be1;
    rag::Corpus first;
    ASSERT_TRUE(first.build(dir.path(), be1.fn(), cfg).dense_ok);

    fs::remove(dir.path() / "a.md");
    FakeBackend be2;
    rag::Corpus second;
    const auto s2 = second.build(dir.path(), be2.fn(), cfg);
    EXPECT_EQ(s2.files_seen, 1u);
    EXPECT_EQ(s2.files_reused, 1u);
    EXPECT_EQ(be2.calls, 0u);   // 无变更无嵌入
    ASSERT_TRUE(s2.dense_ok);
    ASSERT_GT(second.chunk_count(), 0u);
    for (const auto& ch : second.chunks())
        EXPECT_NE(ch.path, "a.md");   // 删除文件的块出局

    // walk 是唯一事实：写回后的缓存里 a.md 也必须消失 ——
    // 再来一个新实例（缓存重放）不该让它复活
    rag::Corpus third;
    third.build(dir.path(), be2.fn(), cfg);
    for (const auto& ch : third.chunks())
        EXPECT_NE(ch.path, "a.md");
}

TEST(CorpusCache, ModelSwitchReembedsAllVectorsButKeepsChunks) {
    TempDir dir;
    write_file(dir.path() / "a.md", kDocA);
    write_file(dir.path() / "b.md", kDocB);
    rag::EmbedConfig cfg1{.model = "m1"};
    FakeBackend be1;
    rag::Corpus first;
    const auto s1 = first.build(dir.path(), be1.fn(), cfg1);
    ASSERT_TRUE(s1.dense_ok);
    const auto total = first.chunk_count();

    // 换模型：同维度不同向量空间（教材 v5 病历），向量全废；块指纹与
    // embed 无关 → 块仍复用（files_reused 不归零）
    rag::EmbedConfig cfg2{.model = "m2"};
    FakeBackend be2;
    rag::Corpus second;
    const auto s2 = second.build(dir.path(), be2.fn(), cfg2);
    EXPECT_EQ(s2.files_reused, 2u);
    EXPECT_EQ(s2.files_rechunked, 0u);
    EXPECT_EQ(s2.chunks_embedded, total);   // 但向量全量重嵌
    ASSERT_TRUE(s2.dense_ok);

    // 新模型身份要落盘：第三次同模型 build 零嵌入
    FakeBackend be3;
    rag::Corpus third;
    const auto s3 = third.build(dir.path(), be3.fn(), cfg2);
    EXPECT_EQ(s3.files_reused, 2u);
    EXPECT_EQ(s3.chunks_embedded, 0u);
    EXPECT_EQ(be3.calls, 0u);
}

TEST(CorpusCache, ChunkerIdentityMismatchInvalidatesAll) {
    TempDir dir;
    write_file(dir.path() / "a.md", kDocA);
    write_file(dir.path() / "b.md", kDocB);
    const rag::EmbedConfig cfg{.model = "fake-embed"};
    FakeBackend be1;
    rag::Corpus first;
    ASSERT_TRUE(first.build(dir.path(), be1.fn(), cfg).dense_ok);
    const auto total = first.chunk_count();

    // 篡改缓存里的 chunker 身份串：等长替换，结构仍合法、身份失配 ——
    // (size,mtime) 指纹对此完全无感，只有身份闸能拦住
    const fs::path cache = dir.path() / std::string{rag::kCorpusCacheName};
    std::string blob = read_file(cache);
    const std::string from{rag::kChunkerIdentity};
    const std::string to = "chunker-vX";
    ASSERT_EQ(from.size(), to.size());
    const auto at = blob.find(from);
    ASSERT_NE(at, std::string::npos);
    blob.replace(at, from.size(), to);
    write_file(cache, blob);

    FakeBackend be2;
    rag::Corpus second;
    const auto s2 = second.build(dir.path(), be2.fn(), cfg);
    EXPECT_EQ(s2.files_reused, 0u);          // 连块带向量整体当 miss
    EXPECT_EQ(s2.files_rechunked, 2u);
    EXPECT_EQ(s2.chunks_embedded, total);
    ASSERT_TRUE(s2.dense_ok);
    EXPECT_EQ(second.chunk_count(), total);  // 内容照常正确
    const auto hits = second.search("zephyr", be2.fn(), cfg, 3);
    ASSERT_FALSE(hits.hits.empty());
    EXPECT_EQ(second.chunks()[hits.hits[0].first].path, "a.md");
}

TEST(CorpusCache, CorruptCacheSilentlyRebuilds) {
    // 三种损坏形态：垃圾头 / 头部截断 / 中段截断（写一半崩溃）
    for (int mode = 0; mode < 3; ++mode) {
        TempDir dir;
        write_file(dir.path() / "a.md", kDocA);
        write_file(dir.path() / "b.md", kDocB);
        const rag::EmbedConfig cfg{.model = "fake-embed"};
        FakeBackend be1;
        rag::Corpus first;
        ASSERT_TRUE(first.build(dir.path(), be1.fn(), cfg).dense_ok);
        const auto total = first.chunk_count();

        const fs::path cache = dir.path() / std::string{rag::kCorpusCacheName};
        std::string blob = read_file(cache);
        ASSERT_FALSE(blob.empty());
        switch (mode) {
            case 0: blob.assign("\xde\xad\xbe\xef garbage"); break;  // 坏 magic
            case 1: blob.resize(7); break;                    // 头都没读完
            case 2: blob.resize(blob.size() / 2); break;      // 中段截断
        }
        write_file(cache, blob);

        // 静默：不抛不崩，当 miss 全量重建，结果正确
        FakeBackend be2;
        rag::Corpus second;
        const auto s2 = second.build(dir.path(), be2.fn(), cfg);
        EXPECT_EQ(s2.files_reused, 0u);
        EXPECT_EQ(s2.files_rechunked, 2u);
        EXPECT_EQ(s2.chunks_embedded, total);
        ASSERT_TRUE(s2.dense_ok);
        EXPECT_EQ(second.chunk_count(), total);
        const auto hits = second.search("zephyr", be2.fn(), cfg, 3);
        ASSERT_FALSE(hits.hits.empty());
        EXPECT_EQ(second.chunks()[hits.hits[0].first].path, "a.md");
    }
}

TEST(CorpusBuild, EmbedFailureDegradesToBm25AndKeepsCache) {
    TempDir dir;
    write_file(dir.path() / "a.md", kDocA);
    write_file(dir.path() / "b.md", kDocB);
    const rag::EmbedConfig cfg{.model = "fake-embed"};
    FakeBackend be1;
    rag::Corpus first;
    ASSERT_TRUE(first.build(dir.path(), be1.fn(), cfg).dense_ok);

    // b.md 变更 + 后端故障：有块要嵌但嵌不成
    write_file(dir.path() / "b.md",
               "# Beta Notes\n\nThe harbor service now uses the wharf "
               "protocol and new ports.\n");
    FakeBackend be_fail;
    be_fail.fail = true;
    rag::Corpus second;
    const auto s2 = second.build(dir.path(), be_fail.fn(), cfg);
    EXPECT_FALSE(s2.dense_ok);               // 降级 BM25-only（不是错误）
    EXPECT_FALSE(second.has_embeddings());   // 不交半截 dense
    ASSERT_GT(second.chunk_count(), 0u);     // 块照常在，检索走词法路
    const auto hits = second.search("zephyr", be_fail.fn(), cfg, 3);
    ASSERT_FALSE(hits.hits.empty());
    EXPECT_EQ(second.chunks()[hits.hits[0].first].path, "a.md");

    // 失败会话不写缓存：旧缓存（a.md 带向量）原样保留 → 下次好后端
    // build 时 a.md 零嵌入。若失败会话写了无向量缓存，a.md 也会被重嵌，
    // 下面的多重集合比较就会多出 a.md 的输入 —— 计数钉不住的，输入钉得住
    FakeBackend be3;
    rag::Corpus third;
    const auto s3 = third.build(dir.path(), be3.fn(), cfg);
    ASSERT_TRUE(s3.dense_ok);
    EXPECT_EQ(s3.files_reused, 1u);   // 只有 a.md
    EXPECT_EQ(s3.files_rechunked, 1u);  // b.md（对缓存而言是变更）
    std::vector<std::string> seen = be3.doc_inputs;
    std::sort(seen.begin(), seen.end());
    EXPECT_FALSE(seen.empty());
    for (const auto& input : seen)
        EXPECT_EQ(input.find("a.md"), std::string::npos) << "不该出现: " << input;
}

TEST(CorpusBuild, Bm25OnlySessionCarriesIdentityThrough) {
    TempDir dir;
    write_file(dir.path() / "a.md", kDocA);
    write_file(dir.path() / "b.md", kDocB);
    rag::EmbedConfig cfg{.model = "m1"};
    FakeBackend be1;
    rag::Corpus first;
    ASSERT_TRUE(first.build(dir.path(), be1.fn(), cfg).dense_ok);

    // BM25-only 会话（忘开 Ollama 的场景）：不嵌不清，向量与身份原样带过
    rag::EmbedConfig no_model{};
    FakeBackend be2;
    rag::Corpus second;
    const auto s2 = second.build(dir.path(), be2.fn(), no_model);
    EXPECT_EQ(s2.files_reused, 2u);
    EXPECT_EQ(s2.chunks_embedded, 0u);
    EXPECT_EQ(be2.calls, 0u);
    ASSERT_TRUE(second.has_embeddings());   // 缓存向量没有被丢

    // 身份穿透：下次带同模型的会话直接命中，零嵌入。若身份被空 model
    // 抹掉，这里会全量重嵌 —— 计数就不再是 0
    FakeBackend be3;
    rag::Corpus third;
    const auto s3 = third.build(dir.path(), be3.fn(), cfg);
    EXPECT_EQ(s3.files_reused, 2u);
    EXPECT_EQ(s3.chunks_embedded, 0u);
    EXPECT_EQ(be3.calls, 0u);
}

TEST(CorpusBuild, FromMemoryMatchesFolderBuild) {
    TempDir dir;
    write_file(dir.path() / "a.md", kDocA);
    write_file(dir.path() / "b.md", kDocB);
    const rag::EmbedConfig cfg{.model = "fake-embed"};
    FakeBackend be1, be2;
    rag::Corpus from_dir;
    const auto s1 = from_dir.build(dir.path(), be1.fn(), cfg);
    ASSERT_TRUE(s1.dense_ok);

    rag::Corpus from_mem;
    const auto s2 =
        from_mem.build_from_memory({{"a.md", kDocA}, {"b.md", kDocB}},
                                   be2.fn(), cfg);
    ASSERT_TRUE(s2.dense_ok);
    EXPECT_EQ(from_mem.chunk_count(), from_dir.chunk_count());
    EXPECT_EQ(s2.chunks_embedded, from_dir.chunk_count());  // 内存路无缓存
    EXPECT_EQ(s2.files_reused, 0u);

    // 检索行为一致：同查询命中序列按块身份逐一对应（chunk id 依赖
    // 遍历顺序，不能直接比 id）
    for (const char* q : {"zephyr", "harbor", "installer"}) {
        const auto h1 = from_dir.search(q, be1.fn(), cfg, 3);
        const auto h2 = from_mem.search(q, be2.fn(), cfg, 3);
        ASSERT_EQ(h1.hits.size(), h2.hits.size());
        for (std::size_t i = 0; i < h1.hits.size(); ++i)
            EXPECT_EQ(chunk_key(from_dir.chunks()[h1.hits[i].first]),
                      chunk_key(from_mem.chunks()[h2.hits[i].first]))
                << "query=" << q << " rank=" << i;
    }
}
