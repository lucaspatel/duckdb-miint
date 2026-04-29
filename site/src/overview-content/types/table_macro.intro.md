SQL-defined table macros — `CREATE MACRO name(...) AS TABLE
(SELECT ...)`. These behave like table functions in `FROM` clauses
but are inlined at parse time and so impose no runtime overhead
beyond the underlying query.

miint uses table macros for two layers of work:

- **User-facing readers** (`read_gff`, `read_jplace`) and observability
  helpers (`miint_warnings`, `genome_coverage`) that compose with
  `read_*` table functions.
- **MassQL transpilation helpers** (the `mzml_*` family) which the
  [`massql` table function](../../table-functions/mass-spec-analysis/massql/)
  emits SQL against. They're documented for completeness but typically
  hidden behind the MassQL surface — see the
  [Mass spectrometry & MassQL guide](../../../guides/massql/) for the
  full picture.
