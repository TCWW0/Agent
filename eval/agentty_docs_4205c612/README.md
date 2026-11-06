# Agentty docs RAG evaluation snapshot

This is a fixed evaluation corpus copied from
[`1ay1/agentty`](https://github.com/1ay1/agentty) at commit
`4205c612d230a8beef343d84bceccd838d552669`.

- `docs/` contains the 160 Markdown documents indexed by `rag_bench`.
- `judgments/main.original.json` and `heldout.original.json` preserve the
  upstream path-level judgments.
- `judgments/main.passages.json` refines the main set with manually reviewed
  path and line-interval evidence. `document_gold` preserves each case's
  original path list for auditability; `rag_bench` consumes only `gold`.
- `heldout.original.json` remains path-only and must not be inspected or tuned
  against before the main-set decision is frozen.
- `LICENSE` is the upstream MIT license.

Passage targets are alternatives: a hit on any annotated interval counts as a
passage hit. Cases that require multiple pieces of evidence are therefore not
yet measuring conjunctive multi-passage coverage.

Run the reviewed main set from the repository root:

```sh
build/rag_bench \
  eval/agentty_docs_4205c612/docs \
  --judgments eval/agentty_docs_4205c612/judgments/main.passages.json
```

`Corpus::build` writes `docs/.my_agent_rag_cache.bin`; it is generated and
ignored. Generated reports belong under `results/` and are also ignored unless
an intentionally reviewed summary is added separately.
