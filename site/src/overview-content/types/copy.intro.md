DuckDB's `COPY ... TO '...' (FORMAT <name>)` exports query results to a
file in a domain-specific format. miint registers writers for the
formats most relevant to microbiome / sequence work: FASTQ, FASTA, SAM,
BAM, Newick (phylogeny) and BIOM (sparse abundance tables).

Each format expects a specific input schema (typically the same
columns produced by the matching `read_*` table function), so the
common pattern is `COPY (SELECT * FROM read_xxx(...)) TO '...'
(FORMAT <name>, ...)`. See each format's page for the required
columns and per-format parameters.

Unlike scalar / table / aggregate functions, COPY format
descriptions are not exposed by `duckdb_functions()` upstream. miint
keeps a small extension-internal registry exposed via the SQL macro
`miint_documented_copy_functions()`, which the docs pipeline reads
alongside the catalog.
