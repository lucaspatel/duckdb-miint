Operations on genomic intervals — `(start, stop)` pairs typically sourced
from `read_alignments` (`position`, `stop_position` columns). Three
families:

- **Aggregation**: `compress_intervals` merges overlapping intervals
  per group into a minimal non-overlapping set. Useful for coverage
  regions, deduplicating ranges, and gap analysis.
- **Slicing**: `alignment_slice` trims alignments to a specific
  `[start, stop)` region, hard-clipping the trimmed CIGAR portions.
- **Per-position depth**: `compute_coverage_depth` returns an array of
  depth values across a reference, derived from CIGAR operations.

Coordinates are 1-based half-open (`[start, stop)`), consistent with
HTSlib and `read_alignments`.
