Mass-spectrometry format readers. mzML and mzXML are the two open
standards for MS data exchange.

- **`read_mzml`** — per-spectrum data (27 columns: identifiers,
  MS-level metadata, precursor info, peak arrays).
- **`read_mzxml`** — same 27-column schema as `read_mzml`; combine
  freely via `UNION ALL`.
- **`read_mzml_chromatograms`** — chromatogram data (TIC, BPC, SRM,
  SIC) from mzML files.

Both per-spectrum readers stream binary peak arrays as `DOUBLE[]`
columns; for per-peak analysis use `UNNEST`. For higher-level mass-spec
operations (peak pairing, isotope patterns, MassQL queries) see the
[mass-spec-analysis](../mass-spec-analysis/) and
[MassQL macros](../../table-macros/) categories.

Stdin not supported (these formats require file seeking).
