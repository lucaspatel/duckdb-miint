SQL-defined scalar macros — `CREATE MACRO name(...) AS (expression)`.
These behave like scalar functions in queries but are inlined at parse
time, so they cost nothing extra at runtime.

miint registers a small set of helpers around mass-spec arithmetic
(`mz_within`, `mz_within_ppm`, `massdefect`, `mz_massdefect_within`)
that are useful both inside the [MassQL transpilation pipeline](../../../guides/massql/)
and in hand-written queries.
