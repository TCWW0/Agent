#include "my_agent/rag/tokenizer.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace my_agent::rag {
namespace {

constexpr std::string_view kTokenizerIdentity =
    "technical-tokenizer-v2-identifier-parts";

std::size_t utf8_seq_len(unsigned char b) {
    if      ((b & 0x80) == 0x00) return 1;
    else if ((b & 0xE0) == 0xC0) return 2;
    else if ((b & 0xF0) == 0xE0) return 3;
    else if ((b & 0xF8) == 0xF0) return 4;
    return 1;   // 非法首字节：按 1 字节容错前进
}

bool is_ascii_upper(char ch) {
    return ch >= 'A' && ch <= 'Z';
}

bool is_ascii_lower(char ch) {
    return ch >= 'a' && ch <= 'z';
}

bool is_ascii_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

bool is_ascii_alnum(char ch) {
    return is_ascii_lower(ch) || is_ascii_upper(ch) || is_ascii_digit(ch);
}

bool is_ascii_alnum_at(std::string_view text, std::size_t offset) {
    if (offset >= text.size()) return false;

    const auto byte = static_cast<unsigned char>(text[offset]);

    return byte <= 0x80 && is_ascii_alnum(static_cast<char>(byte));
}

std::string ascii_lower(std::string_view text) {
    std::string result;
    result.reserve(text.size());

    for (char ch : text) {
        if (is_ascii_upper(ch)) {
            result.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            result.push_back(ch);
        }
    }

    return result;
}

std::size_t scan_ascii_candidate_end(std::string_view text, std::size_t start) {
    std::size_t end = start + 1;

    while (end < text.size()) {
        const auto byte = static_cast<unsigned char>(text[end]);

        if (byte < 0x80 && is_ascii_alnum(static_cast<char>(byte))) {
            ++end;
            continue;
        }

        const bool single_separator =
            (byte == '_' || byte == '-') && is_ascii_alnum_at(text, end + 1);

        if (single_separator) {
            ++end;
            continue;
        }

        const bool namespace_separator =
            byte == ':' && end + 1 < text.size()
            && text[end + 1] == ':' && is_ascii_alnum_at(text, end + 2);

        if (namespace_separator) {
            end += 2;
            continue;
        }

        break;
    }

    return end;
}

// eg: HybridRrfWeights→ Hybrid / Rrf / Weights getHTTPResponseCode→ get / HTTP / Response / Code
// 将一个单词按照大小写边界进行拆分
bool split_camel_segment(std::string_view segment, std::vector<std::string_view>& parts) {
    if (segment.empty()) return false;

    std::size_t start = 0;
    bool had_boundary = false;

    for (std::size_t index = 1; index < segment.size(); ++index) {
        const char previous = segment[index - 1];
        const char current = segment[index];

        const bool lower_to_upper = is_ascii_upper(current)
            && (is_ascii_lower(previous) || is_ascii_digit(previous));

        const bool acronym_to_word =
            is_ascii_upper(previous) && is_ascii_upper(current)
            && index + 1 < segment.size() && is_ascii_lower(segment[index + 1]);

        if (lower_to_upper || acronym_to_word) {
            parts.push_back(segment.substr(start, index - start));

            start = index;
            had_boundary = true;
        }
    }

    parts.push_back(segment.substr(start));

    return had_boundary;
}


void emit_ascii_candidate(std::string_view candidate, std::vector<std::string>& out){
    if (candidate.empty()) {
        return;
    }

    std::vector<std::string_view> parts;
    parts.reserve(4);

    bool has_underscore = false;
    bool has_hyphen = false;
    bool has_namespace = false;
    bool had_case_boundary = false;

    std::size_t segment_start = 0;

    // 1. 处理结构分隔符之间的片段
    auto emit_segment = [&](std::size_t segment_end) {
        if (segment_end <= segment_start) {
            return;
        }

        const std::string_view segment =
            candidate.substr(
                segment_start,
                segment_end - segment_start);

        const std::size_t parts_begin = parts.size();
        const bool split = split_camel_segment(segment, parts);

        if (split && segment.size() != candidate.size()) {
            const auto insert_at =
                parts.begin() + static_cast<std::ptrdiff_t>(parts_begin);
            parts.insert(insert_at, segment);
        }

        had_case_boundary = had_case_boundary || split;
    };

    // 2. 扫描结构分隔符
    for (std::size_t index = 0; index < candidate.size(); ++index) {
        const char ch = candidate[index];

        if (ch == '_') {
            emit_segment(index);
            has_underscore = true;
            segment_start = index + 1;
            continue;
        }

        if (ch == '-') {
            emit_segment(index);
            has_hyphen = true;
            segment_start = index + 1;
            continue;
        }

        if (ch == ':' && index + 1 < candidate.size() && candidate[index + 1] == ':') {
            emit_segment(index);
            has_namespace = true;

            ++index;
            segment_start = index + 1;
        }
    }

    emit_segment(candidate.size());

    const bool standalone_case_identifier =
        had_case_boundary && !has_underscore && !has_hyphen && !has_namespace;

    if (standalone_case_identifier) {
        out.push_back(ascii_lower(candidate));
    }

    for (const std::string_view part : parts) {
        if (part.size() <= 1) {
            continue;
        }

        out.push_back(ascii_lower(part));
    }
}

} // namespace

std::vector<std::string> tokenize(std::string_view text) {
    std::vector<std::string> out;
    std::vector<std::string> run;

    auto flush_run = [&]{
        if (run.size() == 1) {
            out.push_back(std::move(run[0]));
        } else {
            for (std::size_t index = 0; index + 1 < run.size(); ++index) {
                out.push_back(run[index] + run[index + 1]);
            }
        }
        run.clear();
    };

    std::size_t offset = 0;

    while (offset < text.size()) {
        const auto byte = static_cast<unsigned char>(text[offset]);

        if (byte < 0x80 && is_ascii_alnum(static_cast<char>(byte))) {
            flush_run();

            const std::size_t end = scan_ascii_candidate_end(text, offset);
            emit_ascii_candidate(text.substr(offset, end - offset), out);

            offset = end;
        } else if (byte < 0x80) {
            flush_run();
            ++offset;
        } else {
            std::size_t length = utf8_seq_len(byte);

            if (offset +length > text.size()) {
                length = text.size() -offset;
            }

            run.emplace_back(text.substr(offset, length));
            offset += length;
        }
    }
    flush_run();
    return out;
}

std::string_view tokenizer_identity() noexcept {
    return kTokenizerIdentity;
}

} // namespace my_agent::rag
