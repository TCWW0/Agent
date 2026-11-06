#include "my_agent/rag/rag.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace my_agent::rag {
namespace {
constexpr double kK1 = 1.5;         // tf 饱和旋钮
constexpr double kB  = 0.75;
constexpr int    kHeadingBoost = 3; // 面包屑入袋份数

// ── UTF-8：首字节 → 码点字节长度 ───────────────────────────────────────
std::size_t utf8_seq_len(unsigned char b) {
    if      ((b & 0x80) == 0x00) return 1;
    else if ((b & 0xE0) == 0xC0) return 2;
    else if ((b & 0xF0) == 0xE0) return 3;
    else if ((b & 0xF8) == 0xF0) return 4;
    return 1;   // 非法首字节：按 1 字节容错前进
}
} // namespace（B2：内部链接 —— rag.hpp 不声明这些，具名 ns 下的外部符号
  //     会在 #41 的 dense.cpp 出现同名时撞 ODR）

/*
 * 一个简单的分词器，规则如下
 * 1. 按照utf8编码对于数据进行解释
 * 2. 连续常规英文/数字作为一个整体识别，在遇到ASCII范围内非英文/数字时停止，此时已经识别的作为一个token返回
 * 3. 对于非ascii的utf8字，解析到时直接落缓存，在识别到常规ascii字符时/解析完时触发对应字的落袋
 * TODO: 根据实际的场景来进行进一步的优化
 */
void tokenize(std::string_view s, std::vector<std::string>& out) {
    std::string word;               // 常规ASCII编码的字符组成的字
    std::vector<std::string> run;   // CJK 码点段（每元素=一个码点）
    auto flush_word = [&]{
        if (word.size() > 1) out.push_back(word);
        word.clear();
    };
    auto flush_run = [&]{
        if (run.size()==1){
            out.push_back(std::move(run[0]));    // 单独的字原样发出
        } else {
            for (std::size_t k=0;k+1<run.size();++k){
                out.push_back(run[k]+run[k+1]);     // 重叠 bigram
            }
        }
        run.clear();
    };

    std::size_t i=0;
    while(i<s.size()){
        const auto b = static_cast<unsigned char>(s[i]);
        if (b<0x80 && std::isalnum(b)) {    // 属于一个ascii的数字/字母
            flush_run();                    // 脚本切换：先落袋 CJK 段
            word.push_back(static_cast<char>(std::tolower(b)));
            ++i;
        } else if(b<0x80) {     
            flush_word();                   // 标点/空格：双缓冲都断
            flush_run();
            ++i;
        } else {                            // 属于一个 utf8 常规编码字，正常解析
            flush_word();
            std::size_t len = utf8_seq_len(b);
            // TODO: 修改这里的截断逻辑增强容错？
            if (i+len>s.size()) len = s.size()-i;
            run.emplace_back(s.substr(i,len));
            i+=len;
        }
    }
    flush_word();
    flush_run();
}

// chunker辅助
namespace{
// 围栏标记：行首允许空白缩进的 ``` 或 ~~~（对照 agentty is_fenced_code_start）。
// 两种标记都要认、缩进也要认 —— 否则缩进围栏整段不进 fence 状态，
// 软界会在代码中间断开，恰是围栏守卫要防的事。
bool is_fence_marker(std::string_view line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    const std::string_view t = line.substr(i);
    return t.starts_with("```") || t.starts_with("~~~");
}

// 简单判断传入的内容是否为一个空白内容(只包含空格/换行符/制表符)
bool is_blank(std::string_view line){
    // 主动把常见的 \r 和 \n 尾巴去掉再判断
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    return line.find_first_not_of(" \t") == std::string_view::npos;
}

/*
 * 解析当前行的md标题信息，out储存当前行可能的标题内容
 * 返回值返回可能的标题级别，当非0时该行肯定为标题
 */
int parse_heading(std::string_view line,std::string& out) {
    std::size_t i=0;
    while(i<line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    std::size_t h=i;
    while(h<line.size() && line[h]=='#') ++h;
    int level = static_cast<int>(h-i);
    if(level <1||level>6) return 0;
    if(h <line.size() && line[h] != ' '&& line[h]!='\t') return 0;
    while(h<line.size() && (line[h] == ' ' || line[h] =='\t')) ++h;
    std::size_t e = line.size();
    while(e>h && (line[e-1] == ' ' || line[e-1] == '\t'
            || line[e-1] == '#' ||line[e-1] =='\r')) --e;
    out.assign(line.substr(h,e-h));
    return out.empty()?0:level;
}

struct ChunkContext{
    bool in_code_fence = false;
    std::vector<std::pair<int,std::string>> headings;
};

void update_context(std::string_view line,ChunkContext& ctx) {
    if (is_fence_marker(line)){
        ctx.in_code_fence = !ctx.in_code_fence;
        return;
    }
    if (ctx.in_code_fence) return;
    std::string title;
    if (int lvl = parse_heading(line,title);lvl>0){
        while(!ctx.headings.empty()&&ctx.headings.back().first>=lvl)
            ctx.headings.pop_back();
        ctx.headings.emplace_back(lvl,std::move(title));
    }
}

bool is_safe_break(std::string_view line,const ChunkContext& ctx){
    if (ctx.in_code_fence) return false;
    if (is_blank(line)) return true;
    std::string title;
    return parse_heading(line, title)>0;
}

// 面包屑："path › 一级标题 › 二级标题"（› = UTF-8 \xe2\x80\xba）
std::string breadcrumb_context(const std::string& path,const ChunkContext& ctx){
    std::string s = path;
    for (const auto& [lvl,title]:ctx.headings){
        s+=" \xe2\x80\xba ";
        s+=title;
    }
    if (s.size() >256){
        std::size_t cut = 256;
         while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
            --cut;                    // 截断也不能劈开码点
        s.resize(cut);
    }
    return s;
}

} // namespace

std::vector<Chunk> 
chunk_document(const std::string &path, const std::string &body,
                std::size_t max_lines,std::size_t max_chars,std::size_t overlap_lines){
    // 1) 切行
    std::vector<std::string_view> lines;
    {
        std::size_t start =0;
        for(std::size_t i=0;i<=body.size();i++){
            if (i == body.size()||body[i] == '\n'){
                lines.emplace_back(body.data()+start, i-start);
                start = i+1;
            }
        }
    }
    
    // 2) 拆分超大行
    if (max_chars >4){
        std::vector<std::string_view> split;
        split.reserve(lines.size());
        for(std::string_view ln:lines){
            if (ln.size() <= max_chars) {split.push_back(ln);continue;}
            std::size_t off = 0;    // 当前这一子段在原始文本中的位置
            while(ln.size() - off > max_chars){
                std::size_t cut = off + max_chars;      // 本次要切的位置
                std::size_t guard = 0;                  // 在一次切中最大允许的切次数(限制最长utf8字长)
                while(cut > off + 1&& guard < 4 &&(static_cast<unsigned char>(ln[cut]) & 0xC0) == 0x80){
                    --cut; ++guard;
                }
                split.emplace_back(ln.data()+off,cut-off);
                off = cut;
            }
            if (off < ln.size())
                split.emplace_back(ln.data() + off,ln.size()-off);
        }
        lines.swap(split);
    }

    std::vector<Chunk> out;
    std::size_t i=0;
    const std::size_t n = lines.size();
    ChunkContext ctx;

    while (i < n) {
        const std::size_t begin = i;
        std::size_t char_count = 0;
        std::size_t taken = 0;
        ChunkContext chunk_ctx = ctx;   // 本块的工作副本（含自己 span 的标题）

        // 3) 生长循环：双界约束。软界（行数）不许在围栏内断——闭合 ```
        //    会落到下一块；硬界（字符数）不看围栏，兜底未闭合围栏。
        while (i < n) {
            const std::size_t llen = lines[i].size() + 1;   // +1 换行符
            const bool line_overflow = (taken >= max_lines);
            const bool char_overflow = (char_count + llen > max_chars);
            // 如果行数已经达到上限，而且当前不在代码块里，那么不能继续；
            // 或者，如果字节数已经达到上限，那么无论什么情况都不能继续。
            const bool would_overflow =
                (line_overflow && !chunk_ctx.in_code_fence) || char_overflow;

            if (would_overflow && taken > 0) break;

            // 语义断点：块已有内容时，空行/标题之前优先断。
            if (taken > 0 && is_safe_break(lines[i], chunk_ctx)) break;

            update_context(lines[i], chunk_ctx);
            char_count += llen;
            ++taken;
            ++i;

            if (taken >= max_lines && !chunk_ctx.in_code_fence) break;
            if (char_count >= max_chars) break;
        }
        const std::size_t end = i;   // 独占，i指向的是当前chunk最后一行的行号

        // 4) 组装：span 逐行 + '\n'；纯空白块丢弃。
        std::string text;
        text.reserve(char_count);
        for (std::size_t j = begin; j < end; ++j) {
            text.append(lines[j].data(), lines[j].size());
            text.push_back('\n');
        }
        if (text.find_first_not_of(" \t\r\n") != std::string::npos) {
            Chunk c;
            c.path = path;
            c.line_start = static_cast<int>(begin + 1);
            c.line_end   = static_cast<int>(end);
            c.text = std::move(text);
            c.context = breadcrumb_context(path, chunk_ctx);
            out.push_back(std::move(c));
        }

        // 5) overlap：下一块起点回退 overlap_lines 行，必须保证前进。
        std::size_t next;
        if (end < n && overlap_lines > 0 && end > begin + overlap_lines)
            next = end - overlap_lines;
        else
            next = end;
        if (next <= begin) next = begin + 1;

        // 关键坑：全局 ctx 重放到 next 而非 end——重放到 end 会把重叠
        // 行的围栏/标题状态重复 toggle，下一块的起点上下文就损坏了。
        for (std::size_t j = begin; j < next && j < end; ++j)
            update_context(lines[j], ctx);

        i = next;
    }

    return out;
}

Bm25Index build_bm25(const std::vector<Chunk>& chunks) {
    Bm25Index idx;
    idx.doc_count = chunks.size();
    idx.doc_len.assign(chunks.size(),0);

    std::vector<std::string> toks;
    std::uint64_t total_len = 0;

    for (std::uint32_t d =0;d<chunks.size();d++){
        toks.clear();
        tokenize(chunks[d].text, toks);
        // 将面包屑也入袋来抬高相应的权重
        for (int r = 0;r<kHeadingBoost;++r)
            tokenize(chunks[d].context, toks);
        idx.doc_len[d] = static_cast<std::uint32_t>(toks.size());
        total_len += toks.size();

        // 词袋 -> tf 计数 (将重复出现的token合并并且统计在 chunk 内的 tf)
        std::unordered_map<std::uint32_t, std::uint32_t> tf;
        tf.reserve(toks.size());
        for (const auto& token: toks) {
            std::uint32_t id;
            auto it = idx.term_ids.find(token);
            if(it == idx.term_ids.end()){
                id = static_cast<std::uint32_t>(idx.term_ids.size());
                idx.term_ids.emplace(token,id);
                idx.postings.emplace_back();
            } else {
                id = it->second;
            }
            ++tf[id];
        }
        for (const auto& [id,count]:tf)
            idx.postings[id].push_back({d,count});
    }

    idx.avg_doc_len = chunks.empty() ? 0.0
        : static_cast<double>(total_len) / static_cast<double>(chunks.size());
    return idx; 
}

std::vector<std::pair<std::uint32_t, double>>
bm25_search(const Bm25Index& idx, std::string_view query,std::size_t k) {
    std::vector<std::pair<std::uint32_t, double>> out;
    if (idx.doc_count == 0|| k == 0) return out;

    std::vector<std::string> qtoks;
    tokenize(query, qtoks);

    std::unordered_map<std::uint32_t, double> scores;
    const double N = static_cast<double>(idx.doc_count);

    for(const auto& qt: qtoks) {
        auto it = idx.term_ids.find(qt);
        if (it == idx.term_ids.end()) continue;
        const auto& plist= idx.postings[it->second];
        const double df = static_cast<double>(plist.size());
        if (df<=0.0) continue;

        // BM25 IDF: +0.5 平滑，+1 防负，再 clamp ≥0
        double idf = std::log((N-df+0.5)/(df+0.5)+1.0);
        if (idf<0.0) idf = 0.0;

        for(const auto& p:plist){
            const double tf   = static_cast<double>(p.tf);
            const double dl   = static_cast<double>(idx.doc_len[p.chunk_id]);
            const double adl  = idx.avg_doc_len > 0.0 ? idx.avg_doc_len :1.0;
            const double norm = tf * (kK1 + 1.0) / (tf +kK1 * (1.0-kB+kB*dl/adl));
            scores[p.chunk_id]+=idf*norm;
        }
    }

    out.assign(scores.begin(),scores.end());
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){
        if (a.second != b.second) return a.second > b.second;   // 分数降序
        return a.first < b.first;                               // 同分id升序
    });
    if (out.size()>k) out.resize(k);
    return out;
}

} // namespace my_agent::rag
