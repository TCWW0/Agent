#include "my_agent/rag/tokenizer.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace my_agent::rag {
namespace {

constexpr std::size_t kMinAsciiTokenBytes = 2;
constexpr std::size_t kMaxAsciiTokenBytes = 40;
constexpr std::size_t kMaxTokensPerCandidate = 16;

constexpr std::string_view kTokenizerIdentity =
    "technical-tokenizer-v2-bounded-symbolic-atoms";

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

bool is_ascii_letter_at(std::string_view text, std::size_t offset) {
    if (offset >= text.size()) {
        return false;
    }

    return is_ascii_lower(text[offset]) || is_ascii_upper(text[offset]);
}

bool is_ascii_digit_at(std::string_view text, std::size_t offset) {
    return offset < text.size() && is_ascii_digit(text[offset]);
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

struct SymbolicCandidate {
    std::size_t end;
    std::size_t family_bytes = 0; // Only used by version-suffixed C++/C# atoms.
};

std::optional<SymbolicCandidate>
scan_language_candidate(std::string_view text, std::size_t start) {
    if (start >= text.size() || (text[start] != 'C' && text[start] != 'c')) {
        return std::nullopt;
    }

    std::size_t family_end = start;

    if (start + 2 < text.size() && text[start + 1] == '+' && text[start + 2] == '+') {
        family_end = start + 3;
    } else if (start + 1 < text.size() && text[start + 1] == '#') {
        family_end = start + 2;
    } else {
        return std::nullopt;
    }

    std::size_t end = family_end;

    while (is_ascii_digit_at(text, end)) {
        ++end;
    }

    if (is_ascii_alnum_at(text, end)) {
        return std::nullopt;
    }

    return SymbolicCandidate{
        .end = end,
        .family_bytes = family_end - start,
    };
}

std::optional<SymbolicCandidate>
scan_flag_candidate(std::string_view text, std::size_t start) {
    if (start >= text.size() || text[start] != '-') {
        return std::nullopt;
    }

    if (start > 0 && (is_ascii_alnum_at(text, start - 1) || text[start - 1] == '-')) {
        return std::nullopt;
    }

    std::size_t core_start = start + 1;
    if (core_start < text.size() && text[core_start] == '-') {
        ++core_start;
    }

    if (!is_ascii_letter_at(text, core_start)) {
        return std::nullopt;
    }

    std::size_t end = core_start + 1;
    while (end < text.size()) {
        if (is_ascii_alnum_at(text, end) || text[end] == '_') {
            ++end;
            continue;
        }

        if (text[end] == '-' && is_ascii_alnum_at(text, end + 1)) {
            ++end;
            continue;
        }

        break;
    }

    return SymbolicCandidate{.end = end};
}

// Examples: v1.2 and v1.2.30-beta.
std::optional<SymbolicCandidate>
scan_version_candidate(std::string_view text, std::size_t start) {
    if (start >= text.size() ||
        (text[start] != 'v' && text[start] != 'V') || !is_ascii_digit_at(text, start + 1)) {
        return std::nullopt;
    }

    std::size_t end = start + 2;

    while (is_ascii_digit_at(text, end)) {
        ++end;
    }

    bool has_dotted_component = false;

    while (end < text.size() && text[end] == '.' && is_ascii_digit_at(text, end + 1)) {
        has_dotted_component = true;
        end += 2;

        while (is_ascii_digit_at(text, end)) {
            ++end;
        }
    }

    if (!has_dotted_component) {
        return std::nullopt;
    }

    if (end < text.size() && text[end] == '-' && is_ascii_alnum_at(text, end + 1)) {
        ++end;

        while (end < text.size()) {
            if (is_ascii_alnum_at(text, end)) {
                ++end;
                continue;
            }

            const bool suffix_separator = (text[end] == '.' || text[end] == '-')
                                            && is_ascii_alnum_at(text, end + 1);

            if (!suffix_separator) {
                break;
            }

            ++end;
        }
    }
    if (is_ascii_alnum_at(text, end)) {
        return std::nullopt;
    }
    return SymbolicCandidate{.end = end};
}

std::optional<SymbolicCandidate>
scan_http_version_candidate(std::string_view text, std::size_t start) {
    if (start + 5 >= text.size()) {
        return std::nullopt;
    }

    const bool is_http =
        (text[start] == 'H' || text[start] == 'h') &&
        (text[start + 1] == 'T' || text[start + 1] == 't') &&
        (text[start + 2] == 'T' || text[start + 2] == 't') &&
        (text[start + 3] == 'P' || text[start + 3] == 'p');

    if (!is_http || text[start + 4] != '/' || !is_ascii_digit_at(text, start + 5)) {
        return std::nullopt;
    }

    std::size_t end = start + 6;
    while (is_ascii_digit_at(text, end)) {
        ++end;
    }

    while (end < text.size() && text[end] == '.' && is_ascii_digit_at(text, end + 1)) {
        end += 2;

        while (is_ascii_digit_at(text, end)) {
            ++end;
        }
    }

    if (is_ascii_alnum_at(text, end)) {
        return std::nullopt;
    }

    return SymbolicCandidate{.end = end};
}

std::optional<SymbolicCandidate>
scan_symbolic_candidate(std::string_view text, std::size_t start) {
    if (const auto candidate = scan_language_candidate(text, start)) {
        return candidate;
    }

    if (const auto candidate = scan_flag_candidate(text, start)) {
        return candidate;
    }

    if (const auto candidate = scan_version_candidate(text, start)) {
        return candidate;
    }

    if (const auto candidate =
            scan_http_version_candidate(text, start)) {
        return candidate;
    }

    return std::nullopt;
}

struct CandidateEmitter {
    std::vector<std::string>& out;
    std::size_t emitted = 0;

    [[nodiscard]] bool full() const noexcept {
        return emitted >= kMaxTokensPerCandidate;
    }

    void emit(std::string_view token) {
        if (full() || token.size() < kMinAsciiTokenBytes || token.size() > kMaxAsciiTokenBytes) {
            return;
        }

        out.push_back(ascii_lower(token));
        ++emitted;
    }
};

[[nodiscard]] bool is_camel_boundary(std::string_view segment, std::size_t index) {
    const char previous = segment[index - 1];
    const char current = segment[index];

    const bool lower_to_upper =
        (is_ascii_lower(previous) || is_ascii_digit(previous))
        && is_ascii_upper(current);

    const bool acronym_to_word =
        index + 1 < segment.size() && is_ascii_upper(previous)
        && is_ascii_upper(current) && is_ascii_lower(segment[index + 1]);

    return lower_to_upper || acronym_to_word;
}

[[nodiscard]] bool has_camel_boundary(std::string_view segment) {
    for (std::size_t index = 1; index < segment.size(); index++) {
        if (is_camel_boundary(segment, index)) {
            return true;
        }
    }
    return false;
}

void emit_camel_segment(std::string_view segment, CandidateEmitter& emitter) {
    if (segment.empty() || emitter.full()) return;

    if (!has_camel_boundary(segment)) {
        emitter.emit(segment);
        return;
    }

    emitter.emit(segment);
    if (emitter.full()) return;

    std::size_t part_start = 0;
    for (std::size_t index = 1; index < segment.size(); ++index) {
        if (!is_camel_boundary(segment, index)) continue;

        emitter.emit(segment.substr(part_start, index - part_start));

        if (emitter.full()) return;

        part_start = index;
    }

    emitter.emit(segment.substr(part_start));
}

void emit_ascii_candidate(std::string_view candidate, CandidateEmitter& emitter) {
    if (candidate.empty()) {
        return;
    }

    std::size_t segment_start = 0;

    const auto emit_segment = [&](std::size_t segment_end) {
        if (segment_end <= segment_start || emitter.full()) {
            return;
        }

        emit_camel_segment(
            candidate.substr(segment_start, segment_end - segment_start),
            emitter);
    };

    for (std::size_t index = 0; index < candidate.size(); ++index) {
        const char ch = candidate[index];

        if (ch == '_' || ch == '-') {
            emit_segment(index);
            if (emitter.full()) return;

            segment_start = index + 1;
            continue;
        }

        if (ch == ':' && index + 1 < candidate.size() && candidate[index + 1] == ':') {
            emit_segment(index);
            if (emitter.full()) {
                return;
            }
            ++index;
            segment_start = index + 1;
        }
    }
    emit_segment(candidate.size());
}

void emit_legacy_ascii_candidates(std::string_view text, CandidateEmitter& emitter) {
    std::size_t offset = 0;

    while (offset < text.size() && !emitter.full()) {
        if (!is_ascii_alnum_at(text, offset)) {
            ++offset;
            continue;
        }

        const std::size_t end = scan_ascii_candidate_end(text, offset);
        emit_ascii_candidate(text.substr(offset, end - offset), emitter);

        offset = end;
    }
}

void emit_symbolic_candidate(
    std::string_view atom, std::size_t family_bytes, std::vector<std::string>& out) {
    CandidateEmitter emitter{out};
    emitter.emit(atom);

    if (family_bytes > 0 && family_bytes < atom.size()) {
        emitter.emit(atom.substr(0, family_bytes));
    }

    if (emitter.full()) {
        return;
    }

    emit_legacy_ascii_candidates(atom, emitter);
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
        const auto symbolic = scan_symbolic_candidate(text, offset);

        if (symbolic) {
            flush_run();

            emit_symbolic_candidate(
                text.substr(offset, symbolic->end - offset),
                symbolic->family_bytes,
                out);

            offset = symbolic->end;
        } else if (byte < 0x80 && is_ascii_alnum(static_cast<char>(byte))) {
            flush_run();

            const std::size_t end = scan_ascii_candidate_end(text, offset);

            CandidateEmitter emitter{out};

            emit_ascii_candidate(text.substr(offset, end - offset), emitter);

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
