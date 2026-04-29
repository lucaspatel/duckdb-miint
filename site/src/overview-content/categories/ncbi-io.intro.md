Fetch sequences and metadata from NCBI by accession number, via
[E-utilities](https://www.ncbi.nlm.nih.gov/books/NBK25501/). All three
functions accept a single accession or a `VARCHAR[]` of accessions.

- **`read_ncbi`** — GenBank metadata (organism, taxonomy id, length,
  molecule type, update date) without downloading the sequence.
- **`read_ncbi_fasta`** — FASTA sequences in `read_fastx`-compatible
  schema, so NCBI sequences combine freely with local files via
  `UNION ALL`.
- **`read_ncbi_annotation`** — feature annotations (genes, CDS, mRNA,
  …) in `read_gff`-compatible schema.

All three require `httpfs` (auto-loaded) and network access to
`eutils.ncbi.nlm.nih.gov`. Default rate is 3 req/s; pass
`api_key='your_key'` to bump to 10 req/s. Transient failures (429,
5xx) retry with exponential backoff.
