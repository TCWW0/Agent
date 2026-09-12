// 切片 #42（issue #42）Corpus 落盘。接缝契约与错误处理语义见 rag.hpp 的
// Corpus 一节，这里只记实现形状：
//   parse_cache（纯解析：永不抛、永不越界，Truncated 仍带已成功的文件
//   前缀 —— 今天的整丢策略不用它，半截保留要用）
//   → 身份闸（chunker 失配 → 连块带向量全废；embed 失配 → 保块弃向量）
//   → build() 里一行策略：if (load.status != Ok) load.files.clear();
//   将来升级半截保留只动这一行，前两层不动。
// 教材对照：agentty/src/rag/corpus.cpp（kCacheMagic v1→v5 的迭代史是缓存
// 身份设计的病历本；我们的差异：无 HNSW 段（brute cosine 不需要结构
// 签名）、无 tombstone（本片只做 build 时增量）、向量按 chunk-id 稠密
// 对位故不走 Chunk 内嵌向量）。
#include "my_agent/rag/rag.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace my_agent::rag {

namespace fs = std::filesystem;

std::string embed_identity(std::string_view model) {
    if (model.empty()) return {};
    std::string id{model};
    id.push_back('\x1f');
    id += embed_input_text(model, EmbedRole::Doc, "");
    id.push_back('\x1f');
    id += kEmbedInputRecipe;
    return id;
}

namespace {

// ── 缓存文件二进制布局（little-endian，机器本地不跨机）──────────────────
// str = u32 长度 + 裸字节。dim 是咨询字段（每向量自带 vec_len），调试用。
// ┌──────────┬────────────────┬────────────┬──────────┬─────┬────────┐
// │  magic   │ format_version │ chunker_id │ embed_id │ dim │ nfiles │
// ├──────────┼────────────────┼────────────┼──────────┼─────┼────────┤
// │   u32    │      u32       │    str     │   str    │ u32 │  u32   │
// └──────────┴────────────────┴────────────┴──────────┴─────┴────────┘
//                               ↓
//                          File × nfiles
//                               ↓
// ┌──────────┬──────┬───────┬─────────┐
// │ rel_path │ size │ mtime │ nchunks │
// ├──────────┼──────┼───────┼─────────┤
// │   str    │ u64  │  i64  │   u32   │
// └──────────┴──────┴───────┴─────────┘
//                               ↓
//                          Chunk × nchunks
//                               ↓
// ┌────────────┬──────────┬──────┬─────────┬─────────┬───────────┐
// │ line_start │ line_end │ text │ context │ vec_len │ f32[...]  │
// ├────────────┼──────────┼──────┼─────────┼─────────┼───────────┤
// │    i32     │   i32    │ str  │   str   │   u32   │  float[]  │
// └────────────┴──────────┴──────┴─────────┴─────────┴───────────┘
constexpr std::uint32_t kCacheMagic = 0x4741524D;
constexpr std::uint32_t kCacheFormatVersion = 1;
constexpr std::size_t kMaxDocBytes = 4ull * 1024 * 1024;
constexpr std::size_t kMaxCacheBytes = 512ull * 1024 * 1024;

struct CachedFile{
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::vector<Chunk> chunks;
    std::vector<std::vector<float>> vecs;
};

struct CacheLoad{
    enum class Status{ NoFile, Ok, BadMagic, BadVersion, Truncated};
    Status status = Status::NoFile;
    std::string chunker_id;
    std::string embed_id;
    std::unordered_map<std::string, CachedFile> files;
};

// 小端读写。机器本地格式，不跨机：直接 memcpy 本机表示即可。
template <class T>
void put(std::string& b, const T& v){
    b.append(reinterpret_cast<const char*>(&v),sizeof(T));
}

void put_str(std::string& b, std::string_view s) {
    put(b,static_cast<uint32_t>(s.size()));
    b.append(s.data(),s.size());
}

template <class T>
bool get(std::string_view& b,T& v){
    if (b.size() < sizeof(T)) return false;
    std::memcpy(&v,b.data(),sizeof(T));
    b.remove_prefix(sizeof(T));
    return true;
}

bool get_str(std::string_view& b, std::string& s){
    std::uint32_t n = 0;
    if (!get(b,n) || b.size() < n) return false;
    s.assign(b.data(),n);
    b.remove_prefix(n); 
    return true;
}

std::string read_file_bytes(const fs::path& p, std::size_t cap){
    std::ifstream f(p,std::ios::binary);
    std::string s{std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
    if (s.size() > cap) s.resize(cap);
    return s;
}

// ── 第 1 层：纯解析。不碰文件系统、永不抛、永不越界 ─────────────────────
// ★ 与教材的差异①：nfiles/nchunks 数量守卫。损坏文件里的计数字段可能
// 是垃圾巨值，教材直接 reserve 会爆内存；每条记录至少占十几个字节，
// 声称数超过 剩余字节/16 必然是损坏（合法缓存永远碰不到这条线）。
CacheLoad parse_cache(std::string_view b){
    CacheLoad out;
    std::uint32_t magic = 0, version = 0;
    // 调用方保证 b 非空才进来：读不出 magic 或 magic 不符 = 文件损坏，
    // 是 BadMagic 而不是 NoFile（NoFile 只属于「文件不存在」）。
    if (!get(b,magic) || magic != kCacheMagic) {
        out.status = CacheLoad::Status::BadMagic;
        return out;
    }
    if (!get(b,version) || version != kCacheFormatVersion) {
        out.status = CacheLoad::Status::BadVersion;
        return out;
    }
    if (!get_str(b, out.chunker_id) || !get_str(b, out.embed_id)) {
        out.status = CacheLoad::Status::Truncated;
        return out;
    }
    std::uint32_t dim = 0, nfiles = 0;
    if (!get(b,dim) || !get(b,nfiles) ||
        static_cast<std::size_t>(nfiles) > b.size() / 16){
        out.status = CacheLoad::Status::Truncated;
        return out;
    }
    for (std::uint32_t fi = 0; fi < nfiles; fi++){
        std::string path;
        std::uint64_t size = 0;
        std::int64_t mtime = 0;
        std::uint32_t nchunks = 0;
        if (!get_str(b,path) || !get(b,size) || !get(b,mtime) || !get(b,nchunks)
            || static_cast<std::size_t>(nchunks) > b.size() / 16){
            out.status = CacheLoad::Status::Truncated;
            return out;
        }
        CachedFile cf;
        cf.size = size;
        cf.mtime = mtime;
        cf.chunks.reserve(nchunks);
        cf.vecs.resize(nchunks);
        bool ok = true;
        for (std::uint32_t ci = 0; ci < nchunks; ++ci){
            Chunk c;
            c.path = path;
            std::int32_t ls = 0,le = 0;
            std::uint32_t elen = 0;
            if (!get(b,ls) || !get(b,le) || !get_str(b,c.text) || !get_str(b, c.context) || !get(b,elen)){
                ok = false;
                break;
            }
            c.line_start = ls;
            c.line_end = le;
            const std::size_t bytes = static_cast<std::size_t>(elen) * sizeof(float);
            if (elen != 0 && b.size() < bytes) {
                ok = false;
                break;
            }
            if (elen != 0) {
                cf.vecs[ci].resize(elen);
                std::memcpy(cf.vecs[ci].data(),b.data(),bytes);
                b.remove_prefix(bytes);
            }
            cf.chunks.push_back(std::move(c));
        }
        if (!ok){
            out.status = CacheLoad::Status::Truncated;
            return out;
        }
        out.files.emplace(std::move(path),std::move(cf));
    }
    out.status = CacheLoad::Status::Ok;
    return out;
}

// ── 落盘（tmp + rename 原子替换）────────────────────────────────────────
// ★ 与教材的差异②：指纹用 walk 时刻的快照，而不是写盘时重新 stat。
// 全量嵌入要几十秒，build 中途被编辑的文件若按「写盘时的新 stat +
// 旧内容的块」落盘，下次 build 会把它当新鲜货 —— 静默陈旧。教材的
// write_cache_ 每处现场 stat，没躲开这个窗口；我们把 walk 采的指纹
// 传进来，窗口关死。
void write_cache(
    const fs::path& root,const std::vector<Chunk>& chunks,
    const std::vector<std::vector<float>>& vecs, const std::string& embed_id,
    const std::unordered_map<std::string, std::pair<std::uint64_t, std::int64_t>>& fingerprints){
    // 按文件路径分组。vecs 与 chunks 等长由 build 维护，这里直接下标。
    std::unordered_map<std::string, std::vector<std::uint32_t>> by_path;
    for (std::uint32_t i = 0; i < chunks.size(); ++i)
        by_path[chunks[i].path].push_back(i);

    std::uint32_t dim = 0;
    for (const auto& v : vecs)
        if(!v.empty()) { dim = static_cast<std::uint32_t>(v.size()); break;}

    std::string blob;
    put(blob, kCacheMagic);
    put(blob,kCacheFormatVersion);
    put_str(blob,kChunkerIdentity);
    put_str(blob, embed_id);
    put(blob, dim);
    put(blob, static_cast<std::uint32_t>(by_path.size()));
    for (const auto& [path,ids] : by_path) {
        std::uint64_t sz = 0;
        std::int64_t mt = 0;
        if (auto it = fingerprints.find(path); it != fingerprints.end()){
            sz = it->second.first;
            mt = it->second.second;
        } else {    // 理论上不可达（walk 采过快照的文件必在 fingerprints 里）；
                    // 万一竞态丢了，回退现场 stat，宁可指纹不准也不至于崩
            std::error_code ec;
            sz = static_cast<std::uint64_t>(fs::file_size(root / path,ec));
            if (ec) {ec.clear(); sz = 0;}
            const auto t = fs::last_write_time(root / path,ec);
            if (!ec)
                mt = static_cast<std::int64_t>(t.time_since_epoch().count());
            ec.clear();
        }
        put_str(blob, path);
        put(blob,sz);
        put(blob, mt);
        put(blob, static_cast<std::uint32_t>(ids.size()));
        for (std::uint32_t i :ids) {
            const Chunk& c = chunks[i];
            const std::vector<float>& v = vecs[i];
            put(blob, static_cast<std::int32_t>(c.line_start));
            put(blob, static_cast<std::int32_t>(c.line_end));
            put_str(blob, c.text);
            put_str(blob, c.context);
            put(blob,static_cast<std::uint32_t>(v.size()));
            if (!v.empty())
                blob.append(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(float));
        }
    }

    const fs::path final_path = root / std::string{kCorpusCacheName};
    const fs::path tmp_path = root / (std::string{kCorpusCacheName} + ".tmp");
    {
        std::ofstream f(tmp_path,std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(blob.data(),static_cast<std::streamsize>(blob.size()));
    }
    std::error_code ec;
    fs::rename(tmp_path,final_path,ec);
    if (ec) {ec.clear(); fs::remove(tmp_path,ec); ec.clear();}
}

// ── 补嵌：缺向量的块整批交给 embed_corpus ───────────────────────────────
// ★ 与教材的差异③：教材在 Corpus 里重新实现了一遍批量嵌入循环；我们把
// need 子集构造出来交给 #41 的 embed_corpus —— 批量 64、Doc 角色、
// context+'\n'+text、整批失败即 err、跨批维度守卫全在那条路上白拿。
// 返回成功补嵌的块数；std::nullopt = 本次嵌入失败（调用方降级）。
// model 为空 → 0：BM25-only 会话不嵌、也不算失败。
std::optional<std::size_t> embed_missing(
    const std::vector<Chunk>& chunks, std::vector<std::vector<float>>& vecs,
    const EmbedBackend& backend, const EmbedConfig& cfg){
    if (cfg.model.empty()) return 0;
    std::vector<std::uint32_t> need;
    for (std::uint32_t i = 0; i < chunks.size(); ++i)
        if (vecs[i].empty()) need.push_back(i);
    if (need.empty()) return 0;
    std::vector<Chunk> subset;
    subset.reserve(need.size());
    for (std::uint32_t i : need) subset.push_back(chunks[i]);
    auto built = embed_corpus(subset, backend, cfg);
    if (!built) return std::nullopt;
    for (std::size_t j = 0; j < need.size(); ++j)
        vecs[need[j]] = std::move(built->vecs[j]);
    return need.size();
}

bool assemble_dense(DenseIndex& dense, std::vector<std::vector<float>>& vecs) {
    if (vecs.empty()) return false;
    std::size_t dim = 0;
    for (const auto& v: vecs) {
        if (v.empty()) return false;
        if (dim == 0) dim = v.size();
        else if (v.size() != dim) return false;
    }
    dense.vecs = std::move(vecs);
    return true;
}

} // namespace

Corpus::BuildStats Corpus::build(const std::filesystem::path& root,
                                 const EmbedBackend& backend,
                                 const EmbedConfig& cfg) {
    root_ = root;
    chunks_.clear();
    dense_ = DenseIndex();
    bm25_ = Bm25Index{};
    dense_ok_ = false;
    embed_id_.clear();
    BuildStats stats;

    std::error_code ec;
    if (!fs::is_directory(root_,ec)) {
        ec.clear();
        bm25_ = build_bm25(chunks_);
        return stats;
    }

    // ── 第 1 层：读缓存，纯解析（可能是整段垃圾，解析层不在乎）
    CacheLoad load;
    {
        const std::string blob = read_file_bytes(root_ / std::string{kCorpusCacheName}, kMaxCacheBytes);
        if (!blob.empty()) load = parse_cache(blob);
    }

    // ── 第 3 层：策略。「整丢」就是这一行 —— 将来升级半截保留改这里，
    //    解析层和身份闸都不用动。
    if (load.status != CacheLoad::Status::Ok) load.files.clear();

    // ── 第 2 层：身份闸。失效半径按「谁变了谁废」：
    //   chunker 失配 → 块本身就是错块 → 整体当 miss；
    //   embed 失配   → 块仍正确（BM25 侧与 embed 无关）→ 保块弃向量；
    //   model 空     → 不算切换：身份沿用缓存（穿透，不被空 model 抹掉）。
    const std::string session_embed_id = embed_identity(cfg.model);
    embed_id_ = session_embed_id;
    if (!load.files.empty()) {
        if (load.chunker_id != std::string{kChunkerIdentity}) {
            load.files.clear();
        } else if (!cfg.model.empty() && load.embed_id != session_embed_id) {
            for (auto& kv: load.files)
                for (auto& v : kv.second.vecs) v.clear();
        } else if (cfg.model.empty() && !load.embed_id.empty()) {
            embed_id_ = load.embed_id;
        }
    }

    // ── walk：目录是唯一事实。删除语义不单独写代码 —— walk 看不见的
    //    文件进不了 chunks_，写回时自然从缓存消失。
    //    指纹在这里采快照，落盘用（见 write_cache 的 ★②）。
    std::vector<std::vector<float>> vecs;
    std::unordered_map<std::string, std::pair<std::uint64_t, std::int64_t>> fingerprints;
    for (auto it = fs::recursive_directory_iterator(root_,fs::directory_options::skip_permission_denied,ec);
        it != fs::recursive_directory_iterator(); it.increment(ec)){
        if (ec) {ec.clear(); continue;}
        const auto& entry = *it;
        std::error_code e2;
        if (entry.is_directory(e2)) {
            if (entry.path().filename().string().starts_with('.'))
                it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(e2) || entry.path().extension() != ".md")
            continue;

        const std::string rel = fs::relative(entry.path(),root_,e2).string();
        if (e2) {e2.clear(); continue;}
        const auto sz = static_cast<std::uint64_t>(entry.file_size(e2));
        if (e2) {e2.clear(); continue;}
        const auto wt = fs::last_write_time(entry.path(),e2);
        if (e2) {e2.clear(); continue;}
        const auto mtime = static_cast<std::int64_t>(wt.time_since_epoch().count());
        fingerprints[rel] = {sz,mtime};
        ++stats.files_seen;

        // 指纹命中：块与向量原样搬运。身份闸若清过向量，搬的就是空
        // （块照常复用，补嵌阶段负责重新向量）。命中要求块非空：空文件
        // 无缓存记录可谈，走重切（产出 0 块，等价跳过）。
        if (auto cit = load.files.find(rel);
            cit != load.files.end() && cit->second.size == sz &&
            cit->second.mtime == mtime && !cit->second.chunks.empty()){
            for (auto& c:cit->second.chunks) chunks_.push_back(std::move(c));
            vecs.insert(vecs.end(),
                        std::make_move_iterator(cit->second.vecs.begin()),
                        std::make_move_iterator(cit->second.vecs.end()));
            ++stats.files_reused;
            continue;
        }

        const std::string body = read_file_bytes(entry.path(), kMaxDocBytes);
        if (body.empty()) continue;
        for (auto& c: chunk_document(rel, body))
            chunks_.push_back(std::move(c));
        vecs.resize(chunks_.size());
        ++stats.files_rechunked;
    }

    // ── 补嵌变更部分（model 空且有缺块 → 留空，下次会话补）
    const auto filled = embed_missing(chunks_, vecs, backend, cfg);
    bool embed_failed = false;
    if (filled)
        stats.chunks_embedded = *filled;
    else
        embed_failed = true;

    // ── 落盘：embed 失败的会话不动磁盘 —— 旧缓存原样保留（它仍是好的），
    //    环境恢复后直接命中。
    if (!embed_failed)
        write_cache(root_, chunks_, vecs, embed_id_, fingerprints);

    // ── 组装：词法路恒建；语义路按 assemble_dense 的开门条件。
    //    对位不变式在此成立：vecs[i] 与 chunks_[i] 全程同序追加。
    bm25_ = build_bm25(chunks_);
    dense_ok_ = !embed_failed && assemble_dense(dense_, vecs);
    stats.dense_ok = dense_ok_;
    return stats;
}

Corpus::BuildStats Corpus::build_from_memory(
    const std::vector<std::pair<std::string, std::string>>& docs,
    const EmbedBackend& backend, const EmbedConfig& cfg) {
    root_.clear();
    chunks_.clear();
    dense_ = DenseIndex{};
    bm25_ = Bm25Index{};
    dense_ok_ = false;
    embed_id_ = embed_identity(cfg.model);
    BuildStats stats;

    std::vector<std::vector<float>> vecs;
    for (const auto& [path,body]: docs){
        if (body.empty()) continue;
        ++stats.files_seen;
        ++stats.files_rechunked;
        for (auto& c:chunk_document(path, body))
            chunks_.push_back(std::move(c));
        vecs.resize(chunks_.size());
    }

    const auto filled = embed_missing(chunks_, vecs, backend, cfg);
    if (filled) stats.chunks_embedded = *filled;

    bm25_ = build_bm25(chunks_);
    dense_ok_ = filled.has_value() && assemble_dense(dense_, vecs);
    stats.dense_ok = dense_ok_;
    return stats;
}

std::size_t Corpus::chunk_count() const { return chunks_.size(); }
bool Corpus::has_embeddings() const { return dense_ok_; }
const std::vector<Chunk>& Corpus::chunks() const {return chunks_;}
const Bm25Index& Corpus::bm25() const { return bm25_; }
const DenseIndex& Corpus::dense() const { return dense_; }

std::vector<std::pair<std::uint32_t, double>>
Corpus::search(std::string_view query, const EmbedBackend& backend,
               const EmbedConfig& cfg, std::size_t k) const {
    if (chunks_.empty()) return {};
    return hybrid_search(bm25_, dense_, query, backend, cfg, k);
}

} // namespace my_agent::rag
