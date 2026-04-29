Per-row scalar transforms on DNA and RNA sequence strings. Two
families:

- **Reverse complement** (`sequence_dna_reverse_complement`,
  `sequence_rna_reverse_complement`) — reverse the order and complement
  each base. Useful for finding palindromes, building reverse-strand
  pipelines, and normalizing read orientation.

- **IUPAC → regex** (`sequence_dna_as_regexp`,
  `sequence_rna_as_regexp`) — expand ambiguity codes into character
  classes so DuckDB's `regexp_matches` can match degenerate primers
  and probes against `read_fastx` output.

All four functions enforce strict molecular-type validation: the DNA
variants reject `U`, the RNA variants reject `T`. Use the matching
function for your data, or convert with a `replace()` first.
