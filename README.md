# MIINT: MIcrobiome INTelligence.

## Introduction

The MIINT [DuckDB](https://duckdb.org) extension brings columnar analytics to microbiome data. Databases like DuckDB that store data by columns rather than rows allow for greatly increased query performance on analytical tasks, especially those that involve scanning large datasets and aggregating data across a few columns. To enable researchers to exploit these incredible performance improvements, MIINT bridges existing data and tools to DuckDB's rich ecosystem, enabling DuckDB to interoperate with file formats and operations central to microbiome studies.

## Quick Start

```sql
-- Install and load the extension
INSTALL miint FROM community;
LOAD miint;

-- Read alignment files using glob pattern (files sorted alphabetically)
-- Use include_filepath to track source file as sample identifier
CREATE VIEW all_alignments AS
    SELECT *, regexp_extract(filepath, '.*/(.*)\.bam', 1) AS sample_id
    FROM read_alignments('samples/*.bam', include_filepath=true);

-- Filter high-quality primary alignments
CREATE VIEW high_quality AS
    SELECT * FROM all_alignments
    WHERE alignment_is_primary(flags)
      AND mapq >= 30
      AND alignment_query_coverage(cigar) > 0.9;

-- Compute OGU counts using Woltka algorithm
CREATE VIEW ogu_counts AS
    SELECT * FROM woltka_ogu_per_sample(high_quality, sample_id, read_id);

-- Export to BIOM format for downstream analysis (QIIME2, phyloseq, etc.)
COPY (SELECT * FROM ogu_counts)
TO 'ogu_table.biom' (FORMAT BIOM, COMPRESSION 'gzip');

-- Or analyze directly in SQL
SELECT sample_id,
       COUNT(DISTINCT feature_id) AS richness,
       SUM(value) AS total_reads
FROM ogu_counts
GROUP BY sample_id;
```

## Key Capabilities

- **Read bioinformatics formats as tables**: FASTQ/FASTA, SAM/BAM, SFF, BIOM, GFF, Newick trees, jplace, mzML, mzXML
- **Mass spectrometry analysis**: MassQL query language, `read_mzml`/`read_mzxml` with `UNION ALL`-compatible schemas, 30+ helper macros for peak matching, isotope patterns, and cross-level queries
- **Align sequences in SQL**: minimap2 and Bowtie2 integration with sharded parallel alignment, MAFFT multiple sequence alignment
- **Classify sequences**: RYpe minimizer-based sequence classification
- **Analyze with SQL**: Filter, aggregate, join with metadata tables
- **Write back to standard formats**: Export results to FASTQ, FASTA, SAM/BAM, BIOM, Newick
- **Powerful functions**: Sequence identity, coverage, flag checking, Woltka classification, pairwise alignment, chemical formula parsing
- **Performance**: Parallel processing, efficient compression, streaming I/O

## Documentation

The full documentation site lives under [`site/`](site/) and is built
from C++ catalog metadata at every release. Common entry points:

- **Installation & Building** → `site/src/content/docs/getting-started/installation.md`
- **Function reference** (auto-generated from `duckdb_functions()`) →
  `site/src/content/docs/reference/`
  - Table functions, scalar functions, aggregates, COPY formats, macros
- **Guides** → `site/src/content/docs/guides/`
  - Mass spectrometry & MassQL, RYpe sequence classification
- **Internals** → `site/src/content/docs/internals/`
  - Architecture, embedded tools, table/view reading, Arrow zero-copy,
    per-sample pattern, building the docs, testing, WASM testing

To browse locally, see
[`site/src/content/docs/internals/building-the-docs.md`](site/src/content/docs/internals/building-the-docs.md).

## Installing

```sql
INSTALL miint FROM community;
LOAD miint;
```

See `site/src/content/docs/getting-started/installation.md` for
building from source.

## Python CLI

A lightweight Python CLI wraps the extension for common workflows (format conversion, alignment, feature table generation):

```sh
# Install from a local checkout (requires duckdb Python package)
cd python && pip install -e .

# Convert FASTQ to parquet using a local extension build
miint --extension-path ./build/release/extension/miint/miint.duckdb_extension \
    convert sequence -1 reads.fastq.gz -o reads.parquet

# Align and compute OGU feature table
miint align minimap2 -1 reads.parquet -d reference.fasta.gz -o alignments.parquet
miint transform woltka-ogu -i alignments.parquet -o feature_table.parquet
```

See `miint --help` for all commands. Without `--extension-path`, the CLI installs miint from community extensions automatically.

## Running the Tests

```sh
# All tests (SQL, C++, and shell)
bash run_tests.sh

# SQL tests only
make test

# C++ unit tests only
./build/release/extension/miint/tests
```

See `site/src/content/docs/internals/testing.md` for details.

---

This repository is based on https://github.com/duckdb/extension-template, check it out if you want to build and ship your own DuckDB extension.
