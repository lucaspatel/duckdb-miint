Phylogenetic tree readers and tree manipulation. Newick and jplace files
land as relations with one row per tree node — composable with the rest
of SQL the same way alignment data is.

- **`read_newick`** — standard Newick syntax, plus jplace's `{n}` edge
  identifiers.
- **`read_jplace_newick`** — extracts the reference tree from a jplace
  placement file. Schema is `UNION ALL`-compatible with `read_newick`.
- **`tree_resolve_placement`** — inserts placement rows back into a
  reference tree, returning the augmented tree.

Use [`read_jplace`](../../table-macros/) (the macro) to read placement
metadata; combine with `read_jplace_newick` and
`tree_resolve_placement` to materialize per-fragment placement trees.
