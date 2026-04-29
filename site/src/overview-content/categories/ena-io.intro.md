Query and stream data from the EBI European Nucleotide Archive (ENA)
Portal API. Accession types — study (`PRJNA*`/`PRJEB*`/`ERP*`/`SRP*`),
sample (`SAMN*`/`SAME*`), run (`SRR*`/`ERR*`), experiment
(`SRX*`/`ERX*`) — are auto-detected from the prefix and cross-resolved
where needed.

- **`read_ena`** — per-result metadata. Pick `result='read_run'`
  (default), `'sample'`, or `'study'`; pass `fields='...'` to control
  which columns come back.
- **`read_ena_attributes`** — submitter-defined sample attributes (the
  custom key/value pairs not in the Portal's fixed schema). Predicate
  pushdown converts a 33,000-sample XML scan into seconds when filters
  use `tag='X'` on searchable fields.
- **`ena_searchable_fields`** — list of fields the Portal API accepts
  as filter keys for a given `result_type`. Use this to discover what
  works in `WHERE tag='X'` pushdown filters above.
- **`read_ena_sequences`** — stream FASTA/FASTQ/SFF reads with
  run/sample/experiment-accession columns. Supports lateral invocation
  (named args use `=>` syntax instead of `=`).

All four require `httpfs` (auto-loaded) and network access to
`www.ebi.ac.uk` (or `ftp.sra.ebi.ac.uk` for sequence streaming).
Rate-limited to ~3 req/s with retry on 429/5xx.
