Read sequence data — FASTA, FASTQ, BAM/SAM, SFF — into one common
schema (`sequence_index`, `read_id`, `comment`, `sequence1`,
`sequence2`, `qual1`, `qual2`). All readers in this category produce
`UNION ALL`-compatible rows, so heterogeneous inputs (e.g. legacy 454
SFF + current Illumina FASTQ + BAM) can be combined in one query.

For the alignment columns (CIGAR, mapping quality, flags, etc.), see
[`read_alignments`](../alignment-io/read_alignments/) under
**alignment-io** instead.
