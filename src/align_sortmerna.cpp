#include "align_sortmerna.hpp"

#include "align_sortmerna_common.hpp"
#include "documented_function.hpp"
#include "sortmerna_result_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

namespace duckdb {

unique_ptr<FunctionData> AlignSortMeRNATableFunction::Bind(ClientContext &context, TableFunctionBindInput &input,
                                                           vector<LogicalType> &return_types,
                                                           vector<std::string> &names) {
	auto data = make_uniq<Data>();

	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("align_sortmerna: query_table is required");
	}
	data->query_table = input.inputs[0].ToString();
	data->ref_paths = ParseSortMeRNARefPaths(input.named_parameters, "align_sortmerna");
	ParseSortMeRNAConfigParams(input.named_parameters, data->config);

	data->query_schema = ValidateSequenceTableSchema(context, data->query_table);
	if (data->query_schema.has_sequence2 != data->config.paired) {
		throw BinderException("align_sortmerna: query table paired-ness (sequence2 %s) does not match "
		                      "paired=%s; set the paired parameter to match or reshape the query",
		                      data->query_schema.has_sequence2 ? "present" : "absent",
		                      data->config.paired ? "true" : "false");
	}

	for (const auto &n : data->names)
		names.emplace_back(n);
	for (const auto &t : data->types)
		return_types.emplace_back(t);
	return data;
}

unique_ptr<GlobalTableFunctionState> AlignSortMeRNATableFunction::InitGlobal(ClientContext &context,
                                                                             TableFunctionInitInput &input) {
	auto &data = input.bind_data->Cast<Data>();
	auto gstate = make_uniq<GlobalState>();

	miint::SortMeRNAConfig cfg = data.config;
	if (cfg.num_threads <= 0) {
		cfg.num_threads = NumericCast<int32_t>(TaskScheduler::GetScheduler(context).NumberOfThreads());
	}
	gstate->aligner = std::make_unique<miint::SortMeRNAAligner>(cfg, data.ref_paths);
	gstate->query_stream = std::make_unique<QuerySequenceStream>(context, data.query_table, data.query_schema);
	// Execute holds gstate.lock across align() on the assumption that
	// MaxThreads() is 1. If that invariant ever changes, the shared
	// result_buffer's deposit path needs a rethink before this body is run
	// concurrently.
	D_ASSERT(gstate->MaxThreads() == 1);
	return gstate;
}

unique_ptr<LocalTableFunctionState> AlignSortMeRNATableFunction::InitLocal(ExecutionContext &, TableFunctionInitInput &,
                                                                           GlobalTableFunctionState *) {
	return make_uniq<LocalState>();
}

void AlignSortMeRNATableFunction::Execute(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<GlobalState>();
	auto &bind_data = data_p.bind_data->Cast<Data>();

	// See the parallel note in AlignSortMeRNARRNATableFunction::Execute. The
	// lock is defensive against a future MaxThreads() change; today the
	// D_ASSERT in InitGlobal guarantees only one DuckDB thread runs this body.
	std::lock_guard<std::mutex> lock(gstate.lock);

	while (true) {
		idx_t available = gstate.result_buffer.size() - gstate.buffer_offset;
		if (available > 0) {
			idx_t count = std::min(available, static_cast<idx_t>(STANDARD_VECTOR_SIZE));
			OutputSortMeRNASamBatch(output, gstate.result_buffer, gstate.buffer_offset, count, bind_data.config.paired);
			gstate.buffer_offset += count;
			return;
		}

		gstate.result_buffer.clear();
		gstate.buffer_offset = 0;

		auto query_batch = gstate.query_stream->FetchSubBatch();
		if (query_batch.empty()) {
			output.SetCardinality(0);
			return;
		}

		miint::SortMeRNAQueryBatch queries;
		queries.read_ids = std::move(query_batch.read_ids);
		queries.sequences = std::move(query_batch.sequences1);
		if (bind_data.config.paired) {
			queries.sequences2 = std::move(query_batch.sequences2);
		}

		try {
			gstate.aligner->align(queries, gstate.result_buffer);
		} catch (const std::exception &e) {
			throw IOException("align_sortmerna: %s", e.what());
		}
	}
}

TableFunction AlignSortMeRNATableFunction::GetFunction() {
	auto tf = TableFunction("align_sortmerna", {LogicalType::VARCHAR}, Execute, Bind, InitGlobal, InitLocal);
	RegisterSortMeRNANamedParameters(tf);
	// See the NO_ORDER note in AlignSortMeRNARRNATableFunction::GetFunction.
	tf.order_preservation_type = OrderPreservationType::NO_ORDER;
	return tf;
}

void AlignSortMeRNATableFunction::Register(ExtensionLoader &loader) {
	static const std::string description = R"DOC(
rRNA filtering / alignment against one or more rRNA reference
databases using [SortMeRNA](https://github.com/biocore/sortmerna)
(Kopylova et al. 2012, *Bioinformatics* 28:3211-3217), embedded as a
statically linked library (SortMeRNA 4.4.0 fork; LGPL-3.0-or-later).

Emits the standard 21-column SAM schema shared with
[`align_minimap2`](../align_minimap2/) /
[`align_bowtie2`](../align_bowtie2/), so results compose freely with
[`read_alignments`](../read_alignments/) output. For SortMeRNA's
native identity / coverage / e-value / edit-distance schema, use
[`align_sortmerna_rrna`](../align_sortmerna_rrna/).

### Required named parameters

- `ref_paths` (VARCHAR[]) — list of FASTA paths for the rRNA reference
  database(s). Index is built once per query in-memory (rebuilt every
  call — no on-disk index format).

### Optional named parameters

- `num_threads` (INTEGER, default = DuckDB's thread count) — internal
  SortMeRNA thread pool size. The DuckDB function itself runs on a
  single thread.
- `match`, `mismatch`, `gap_open`, `gap_ext`, `score_N` (INTEGER) —
  Smith-Waterman scoring. Defaults `2 / -3 / 5 / 2 / 0`.
- `evalue` (DOUBLE, default `1.0`) — e-value threshold.
- `seed_win_len` (UINTEGER, default `18`) — seed window length.
- `num_alignments` (UINTEGER, default `1`) — max alignments per read.
- `best` (BOOLEAN, default `true`) — keep only best-scoring hit per
  read.
- `paired` (BOOLEAN, default `false`) — paired-end mode. Requires a
  `sequence2` column on `query_table`.
- `forward_only`, `reverse_only`, `full_search` (BOOLEAN) —
  strand-search controls.

### SortMeRNA-specific output notes

- `mapq` is always `255` (SortMeRNA does not compute mapping quality;
  255 is the SAM convention for "unavailable").
- `tag_as` carries the raw Smith-Waterman score; `tag_nm` carries
  edit distance. Both NULL for unaligned rows.
- `tag_xs`, `tag_ys`, `tag_xn`, `tag_xm`, `tag_xo`, `tag_xg`,
  `tag_yt`, `tag_md`, `tag_sa` are always NULL.

### Caveats

- **No minimum-score filter**: the embedded library returns every
  positive Smith-Waterman hit. The `sortmerna` CLI applies an internal
  threshold that this streaming API path bypasses. Filter on `tag_as`
  (score) or `e_value` in SQL to reproduce CLI output.
- **E-values diverge from the CLI**: per-query Karlin-Altschul here vs
  database-summed in the CLI. Identity / coverage / score / CIGAR /
  positions / edit distance are bit-identical.
- **Process-wide serialization**: SortMeRNA's `g_run_mutex` serializes
  all calls process-wide. Concurrent `align_sortmerna` /
  `align_sortmerna_rrna` queries block on each other.
)DOC";

	RegisterDocumentedTableFunction(
	    loader, GetFunction(), description, {"query_table"},
	    {
	        "-- Filter reads to those that align to any SILVA reference\n"
	        "CREATE TABLE reads AS SELECT read_id, sequence1 FROM read_fastx('metaT.fastq.gz');\n"
	        "CREATE TABLE rrna_reads AS\n"
	        "  SELECT read_id, flags, reference, position, cigar, tag_as AS score\n"
	        "  FROM align_sortmerna('reads',\n"
	        "         ref_paths := ['silva-bac-16s.fasta', 'silva-arc-16s.fasta'])\n"
	        "  WHERE (flags & 0x4) = 0;",
	    },
	    /*alias_of=*/"", /*categories=*/{"alignment-tools"});
}

} // namespace duckdb
