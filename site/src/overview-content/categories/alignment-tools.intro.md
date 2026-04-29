Run sequence aligners directly from SQL: read query (and reference)
sequences from DuckDB tables/views, get alignments back as a row stream
in the standard 21-column SAM schema (or each tool's native schema for
SortMeRNA's rRNA mode). Results compose freely with
[`read_alignments`](../../table-functions/alignment-io/read_alignments/)
output via `UNION ALL`.

- **Embedded, no binary required** — `align_minimap2`,
  `align_minimap2_sharded`, `save_minimap2_index`, `align_mafft`,
  `align_sortmerna`, `align_sortmerna_rrna` link the upstream library
  statically.
- **Subprocess** — `align_bowtie2` and `align_bowtie2_sharded` shell out
  to `bowtie2`/`bowtie2-build`. Gate calls on
  [`bowtie2_available()`](../../scalar-functions/alignment-tools/bowtie2_available/).
- **Sharded variants** route reads to per-shard pre-built indexes via a
  `(read_id, shard_name)` mapping table — designed for large-scale
  metagenomic workflows where reads are pre-classified.

Output order is non-deterministic across all of these (`NO_ORDER`) so
DuckDB can parallelize CTAS pipelines; `ORDER BY` if you need stable
ordering for downstream comparison.
