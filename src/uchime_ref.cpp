#include "uchime_ref.hpp"
#include "documented_function.hpp"
#include "table_function_common.hpp"
#include "uchime_common.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <algorithm>

namespace duckdb {

// Serves a chunk from the local-state buffer when working in per-sample mode, or
// directly into output at col 0 when not. Returns true if a chunk was emitted (and
// output.cardinality is set); false if the buffer is drained.
static bool EmitFromBuffer(const std::vector<miint::UchimeResult> &buffer, idx_t &result_offset, DataChunk &output,
                           bool has_sample_id, const Value &sample_value) {
	if (result_offset >= buffer.size()) {
		return false;
	}
	idx_t remaining = buffer.size() - result_offset;
	idx_t count = std::min(remaining, static_cast<idx_t>(STANDARD_VECTOR_SIZE));
	if (has_sample_id) {
		output.data[0].Reference(sample_value);
		OutputUchimeResults(output, buffer, result_offset, count, /*start_col=*/1);
	} else {
		OutputUchimeResults(output, buffer, result_offset, count);
	}
	result_offset += count;
	return true;
}

unique_ptr<FunctionData> UchimeRefTableFunction::Bind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<std::string> &names) {
	auto data = make_uniq<Data>();

	data->query_table = input.inputs[0].GetValue<std::string>();

	auto db_it = input.named_parameters.find("db");
	if (db_it == input.named_parameters.end()) {
		throw BinderException("detect_chimera_uchime requires 'db' parameter (reference table name)");
	}
	data->ref_table = db_it->second.GetValue<std::string>();

	data->query_schema = ValidateSequenceTableSchema(context, data->query_table);
	data->ref_schema = ValidateSequenceTableSchema(context, data->ref_table);

	auto get_double = [&](const std::string &name, double &out, double min_val, const char *constraint) {
		auto it = input.named_parameters.find(name);
		if (it != input.named_parameters.end()) {
			out = it->second.GetValue<double>();
			if (out < min_val) {
				throw InvalidInputException("detect_chimera_uchime: %s must be %s (got %g)", name, constraint, out);
			}
		}
	};
	auto get_int = [&](const std::string &name, int &out, int min_val, const char *constraint) {
		auto it = input.named_parameters.find(name);
		if (it != input.named_parameters.end()) {
			out = it->second.GetValue<int>();
			if (out < min_val) {
				throw InvalidInputException("detect_chimera_uchime: %s must be %s (got %d)", name, constraint, out);
			}
		}
	};

	get_double("minh", data->params.minh, 0.0, ">= 0");
	get_double("xn", data->params.xn, 1.0, ">= 1.0");
	get_double("dn", data->params.dn, 0.0, ">= 0");
	get_double("mindiv", data->params.mindiv, 0.0, ">= 0");
	get_int("mindiffs", data->params.mindiffs, 1, ">= 1");

	auto sample_it = input.named_parameters.find("sample_id");
	if (sample_it != input.named_parameters.end()) {
		data->has_sample_id = true;
		data->sample_info.sample_id_col = sample_it->second.GetValue<string>();
	}

	data->names = GetUchimeOutputNames();
	data->types = GetUchimeOutputTypes();

	if (data->has_sample_id) {
		auto &db = DatabaseInstance::GetDatabase(context);
		Connection conn(db);
		// Reserved = 18 uchime output column names (case-insensitive).
		DiscoverSamples(conn, data->query_table, data->sample_info.sample_id_col, data->names, "detect_chimera_uchime",
		                data->sample_info);

		names.push_back(data->sample_info.sample_id_col);
		return_types.push_back(data->sample_info.sample_id_type);
	}

	for (auto &n : data->names) {
		names.push_back(n);
	}
	for (auto &t : data->types) {
		return_types.push_back(t);
	}

	return data;
}

unique_ptr<GlobalTableFunctionState> UchimeRefTableFunction::InitGlobal(ClientContext &context,
                                                                        TableFunctionInitInput &input) {
	auto &data = input.bind_data->Cast<Data>();
	auto gstate = make_uniq<GlobalState>();

	gstate->wrapper = miint::VsearchChimeraWrapper(data.params);

	// Reference load is always global — shared read-only by every sample's detect_batch.
	auto ref = LoadSingleEndSequences(context, data.ref_table, "detect_chimera_uchime");
	gstate->wrapper.set_reference(ref.labels, ref.sequences);

	if (data.has_sample_id) {
		// detect_batch is documented as not thread-safe on the same wrapper instance.
		InitPerSampleGlobal(context, *gstate, data.sample_info.sample_values.size(), /*max_threads_hint=*/1);
		return gstate;
	}

	gstate->max_threads = 1;
	gstate->query_stream =
	    std::make_unique<QuerySequenceStream>(context, data.query_table, data.query_schema, STANDARD_VECTOR_SIZE);
	return gstate;
}

unique_ptr<LocalTableFunctionState> UchimeRefTableFunction::InitLocal(ExecutionContext &context,
                                                                      TableFunctionInitInput &input,
                                                                      GlobalTableFunctionState * /*gstate*/) {
	auto &data = input.bind_data->Cast<Data>();
	auto lstate = make_uniq<LocalState>();
	if (data.has_sample_id) {
		auto &db = DatabaseInstance::GetDatabase(context.client);
		lstate->conn = make_uniq<Connection>(db);
	}
	return lstate;
}

void UchimeRefTableFunction::Execute(ClientContext & /*context*/, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<Data>();
	auto &gstate = data_p.global_state->Cast<GlobalState>();
	auto &lstate = data_p.local_state->Cast<LocalState>();

	if (!data.has_sample_id) {
		while (true) {
			if (EmitFromBuffer(gstate.result_buffer, gstate.result_offset, output, /*has_sample_id=*/false, Value())) {
				return;
			}
			gstate.result_buffer.clear();
			gstate.result_offset = 0;

			auto query_batch = gstate.query_stream->FetchSubBatch();
			if (query_batch.empty()) {
				output.SetCardinality(0);
				return;
			}
			gstate.wrapper.detect_batch(query_batch.read_ids, query_batch.sequences1, gstate.result_buffer);
		}
	}

	while (true) {
		if (EmitFromBuffer(lstate.result_buffer, lstate.result_offset, output, /*has_sample_id=*/true,
		                   lstate.sample_value)) {
			return;
		}
		lstate.result_buffer.clear();
		lstate.result_offset = 0;

		// Drain the current sample's stream, if any.
		if (lstate.query_stream) {
			auto query_batch = lstate.query_stream->FetchSubBatch();
			if (!query_batch.empty()) {
				gstate.wrapper.detect_batch(query_batch.read_ids, query_batch.sequences1, lstate.result_buffer);
				continue;
			}
			lstate.query_stream.reset();
		}

		// Current sample exhausted; claim the next.
		idx_t sample_idx;
		if (!ClaimNextSample(gstate, data.sample_info.sample_values.size(), sample_idx)) {
			output.SetCardinality(0);
			return;
		}
		lstate.sample_value = data.sample_info.sample_values[sample_idx];
		auto sample_literal = lstate.sample_value.ToSQLString();
		auto q_col = KeywordHelper::WriteOptionallyQuoted(data.sample_info.sample_id_col);
		auto q_src = KeywordHelper::WriteOptionallyQuoted(data.query_table);
		// Build a per-sample TEMP VIEW on the thread's connection, then hand that same
		// connection to QuerySequenceStream so the stream can resolve the view. The
		// CAST-as-VARCHAR equality matches the other per-sample call sites; DECIMAL caveat
		// applies (see note in deblur_table_function.cpp).
		auto view_sql = "CREATE OR REPLACE TEMP VIEW __uchime_per_sample AS SELECT * FROM " + q_src + " WHERE CAST(" +
		                q_col + " AS VARCHAR) = CAST(" + sample_literal + " AS VARCHAR)";
		auto view_result = lstate.conn->Query(view_sql);
		if (view_result->HasError()) {
			throw InvalidInputException("detect_chimera_uchime: failed to create per-sample view for %s: %s",
			                            sample_literal, view_result->GetError());
		}
		lstate.query_stream = std::make_unique<QuerySequenceStream>(*lstate.conn, "__uchime_per_sample",
		                                                            data.query_schema, STANDARD_VECTOR_SIZE);
	}
}

TableFunction UchimeRefTableFunction::GetFunction() {
	auto tf = TableFunction("detect_chimera_uchime", {LogicalType::VARCHAR}, Execute, Bind, InitGlobal, InitLocal);

	tf.named_parameters["db"] = LogicalType::VARCHAR;
	tf.named_parameters["minh"] = LogicalType::DOUBLE;
	tf.named_parameters["xn"] = LogicalType::DOUBLE;
	tf.named_parameters["dn"] = LogicalType::DOUBLE;
	tf.named_parameters["mindiv"] = LogicalType::DOUBLE;
	tf.named_parameters["mindiffs"] = LogicalType::INTEGER;
	tf.named_parameters["sample_id"] = LogicalType::VARCHAR;

	tf.order_preservation_type = OrderPreservationType::NO_ORDER;

	return tf;
}

void UchimeRefTableFunction::Register(ExtensionLoader &loader) {
	static const std::string description = R"DOC(
Reference-based chimera detection using the
[UCHIME](https://drive5.com/uchime/) algorithm
(Edgar et al. 2011, *Bioinformatics* 27:2194-2200), powered by the
[vsearch](https://github.com/torognes/vsearch) library
(Rognes et al. 2016, *PeerJ* 4:e2584). Detects chimeric sequences by
comparing each query against a trusted chimera-free reference
database.

Both `query_table` and the table named by `db` need `read_id`
(VARCHAR) and `sequence1` (VARCHAR) columns. One row is emitted per
query — non-chimeras have NULL parent / identity columns and `0` for
the vote columns.

### Required named parameters

- `db` (VARCHAR) — table/view of trusted reference sequences.

### Optional named parameters

- `minh` (DOUBLE, default `0.28`) — minimum h-score to flag chimeric
  (range `[0, 1]`).
- `xn` (DOUBLE, default `8.0`) — weight of "no" votes (≥ `1.0`).
- `dn` (DOUBLE, default `1.4`) — pseudo-count prior on "no" votes
  (≥ `0`).
- `mindiv` (DOUBLE, default `0.8`) — minimum divergence (percentage
  points) from closest parent (≥ `0`).
- `mindiffs` (INTEGER, default `3`) — minimum diffs in each segment
  (≥ `1`).
- `sample_id` (VARCHAR) — column to partition by. The shared
  reference is loaded once; queries score per-sample. Execution is
  serialized (vsearch wrapper is not concurrent-safe).

### Output schema (18 columns, vsearch `--uchimeout`-compatible)

`score`, `query_id`, `parent_a_id`, `parent_b_id`,
`closest_parent_id`, `id_query_model`, `id_query_a`, `id_query_b`,
`id_a_b`, `id_query_top`, `left_yes`, `left_no`, `left_abstain`,
`right_yes`, `right_no`, `right_abstain`, `divergence`, `flag`
(`Y` / `N` / `?`).

`id_a_b` is computed only for chimeric (`Y`) and borderline (`?`)
results — non-chimeras report `0.0` (vsearch computes it
unconditionally; this skips an extra alignment per query).
)DOC";

	RegisterDocumentedTableFunction(
	    loader, GetFunction(), description, {"query_table"},
	    {
	        "-- Detect chimeras against a curated reference database\n"
	        "CREATE TABLE refs AS SELECT read_id, sequence1 FROM read_fastx('gold.fasta');\n"
	        "CREATE TABLE queries AS SELECT read_id, sequence1 FROM read_fastx('amplicons.fasta');\n"
	        "SELECT * FROM detect_chimera_uchime('queries', db := 'refs');",
	        "-- Keep only sequences classified as non-chimeric\n"
	        "CREATE TABLE clean_seqs AS\n"
	        "SELECT q.* FROM queries q\n"
	        "JOIN detect_chimera_uchime('queries', db := 'refs') u ON q.read_id = u.query_id\n"
	        "WHERE u.flag = 'N';",
	        "-- Count classifications\n"
	        "SELECT flag, count(*) FROM detect_chimera_uchime('queries', db := 'refs')\n"
	        "GROUP BY flag;",
	    },
	    /*alias_of=*/"", /*categories=*/{"microbiome"});
}

} // namespace duckdb
