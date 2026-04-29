#include "cluster_sequences.hpp"
#include "documented_function.hpp"
#include "sequence_table_reader.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <algorithm>

namespace duckdb {

static std::vector<std::string> GetClusterOutputNames() {
	return {"read_id", "is_centroid", "cluster_id", "centroid_id", "identity", "cigar", "cigar_truncated"};
}

static std::vector<LogicalType> GetClusterOutputTypes() {
	return {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::INTEGER, LogicalType::VARCHAR,
	        LogicalType::DOUBLE,  LogicalType::VARCHAR, LogicalType::BOOLEAN};
}

static idx_t OutputClusterResults(DataChunk &output, const std::vector<miint::ClusterResult> &results, idx_t offset,
                                  idx_t count) {
	idx_t actual = std::min(count, static_cast<idx_t>(results.size()) - offset);
	if (actual == 0) {
		output.SetCardinality(0);
		return 0;
	}

	idx_t col = 0;

	auto &read_id_vec = output.data[col++];
	for (idx_t i = 0; i < actual; i++) {
		FlatVector::GetData<string_t>(read_id_vec)[i] =
		    StringVector::AddString(read_id_vec, results[offset + i].read_id);
	}

	auto is_centroid_data = FlatVector::GetData<bool>(output.data[col++]);
	for (idx_t i = 0; i < actual; i++) {
		is_centroid_data[i] = results[offset + i].is_centroid;
	}

	auto cluster_id_data = FlatVector::GetData<int32_t>(output.data[col++]);
	for (idx_t i = 0; i < actual; i++) {
		cluster_id_data[i] = results[offset + i].cluster_id;
	}

	auto &centroid_id_vec = output.data[col++];
	for (idx_t i = 0; i < actual; i++) {
		FlatVector::GetData<string_t>(centroid_id_vec)[i] =
		    StringVector::AddString(centroid_id_vec, results[offset + i].centroid_id);
	}

	auto identity_data = FlatVector::GetData<double>(output.data[col++]);
	for (idx_t i = 0; i < actual; i++) {
		identity_data[i] = results[offset + i].identity;
	}

	auto &cigar_vec = output.data[col++];
	for (idx_t i = 0; i < actual; i++) {
		FlatVector::GetData<string_t>(cigar_vec)[i] = StringVector::AddString(cigar_vec, results[offset + i].cigar);
	}

	auto cigar_trunc_data = FlatVector::GetData<bool>(output.data[col++]);
	for (idx_t i = 0; i < actual; i++) {
		cigar_trunc_data[i] = results[offset + i].cigar_truncated;
	}

	D_ASSERT(col == output.ColumnCount());
	output.SetCardinality(actual);
	return actual;
}

unique_ptr<FunctionData> ClusterSequencesTableFunction::Bind(ClientContext &context, TableFunctionBindInput &input,
                                                             vector<LogicalType> &return_types,
                                                             vector<std::string> &names) {
	auto data = make_uniq<Data>();

	data->input_table = input.inputs[0].GetValue<std::string>();

	// Validate table has read_id (VARCHAR) and sequence1 (VARCHAR)
	ValidateSequenceTableSchema(context, data->input_table);

	// id is required — no silent default.
	auto id_it = input.named_parameters.find("id");
	if (id_it == input.named_parameters.end()) {
		throw BinderException("cluster_sequences_vsearch requires 'id' parameter (identity threshold, e.g. id:=0.97)");
	}
	data->params.id = id_it->second.GetValue<double>();
	if (data->params.id < 0.0 || data->params.id > 1.0) {
		throw InvalidInputException("cluster_sequences_vsearch: id must be between 0.0 and 1.0 (got %g)",
		                            data->params.id);
	}

	// strand: 'plus' (default) or 'both'
	auto strand_it = input.named_parameters.find("strand");
	if (strand_it != input.named_parameters.end()) {
		auto strand_str = strand_it->second.GetValue<std::string>();
		if (strand_str == "plus") {
			data->params.strand = 1;
		} else if (strand_str == "both") {
			data->params.strand = 2;
		} else {
			throw InvalidInputException("cluster_sequences_vsearch: strand must be 'plus' or 'both' (got '%s')",
			                            strand_str);
		}
	}

	data->names = GetClusterOutputNames();
	data->types = GetClusterOutputTypes();
	for (auto &n : data->names) {
		names.push_back(n);
	}
	for (auto &t : data->types) {
		return_types.push_back(t);
	}

	return data;
}

unique_ptr<GlobalTableFunctionState> ClusterSequencesTableFunction::InitGlobal(ClientContext &context,
                                                                               TableFunctionInitInput &input) {
	auto &data = input.bind_data->Cast<Data>();
	auto gstate = make_uniq<GlobalState>();

	// Clustering is materialized upfront: vsearch's clustering algorithm is
	// inherently sequential (each centroid must be indexed before the next
	// sequence is processed), and the entire session must complete before
	// releasing the vsearch mutex. Execute() paginates the results.
	miint::VsearchClusterWrapper wrapper(data.params);

	auto loaded = LoadSingleEndSequences(context, data.input_table, "cluster_sequences_vsearch", /*strict=*/true);
	wrapper.set_sequences(loaded.labels, loaded.sequences);
	gstate->results = wrapper.cluster_all();

	return gstate;
}

void ClusterSequencesTableFunction::Execute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<GlobalState>();

	if (gstate.result_offset >= gstate.results.size()) {
		output.SetCardinality(0);
		return;
	}

	idx_t remaining = gstate.results.size() - gstate.result_offset;
	idx_t count = std::min(remaining, static_cast<idx_t>(STANDARD_VECTOR_SIZE));
	OutputClusterResults(output, gstate.results, gstate.result_offset, count);
	gstate.result_offset += count;
}

TableFunction ClusterSequencesTableFunction::GetFunction() {
	auto tf = TableFunction("cluster_sequences_vsearch", {LogicalType::VARCHAR}, Execute, Bind, InitGlobal);

	tf.named_parameters["id"] = LogicalType::DOUBLE;
	tf.named_parameters["strand"] = LogicalType::VARCHAR;

	tf.order_preservation_type = OrderPreservationType::NO_ORDER;

	return tf;
}

void ClusterSequencesTableFunction::Register(ExtensionLoader &loader) {
	static const std::string description = R"DOC(
Greedy sequence clustering, powered by the
[vsearch](https://github.com/torognes/vsearch) library
(Rognes et al. 2016, *PeerJ* 4:e2584). Each input row is compared
against existing cluster centroids in input order — it joins the
best-matching cluster (above the identity threshold) or becomes a new
centroid.

**Sort order is the caller's responsibility.** For
`vsearch --cluster_fast` semantics, sort by length descending. For
`vsearch --cluster_size` (most-abundant first), sort by abundance
descending.

The input table must have `read_id` (VARCHAR) and `sequence1`
(VARCHAR) columns.

### Required named parameters

- `id` (DOUBLE) — minimum identity (0.0–1.0). No silent default.

### Optional named parameters

- `strand` (VARCHAR, default `'plus'`) — `'plus'` for plus-strand
  only, `'both'` to also search reverse complements.

### Output schema

`read_id`, `is_centroid` (BOOLEAN), `cluster_id` (INTEGER, 0-based),
`centroid_id` (VARCHAR), `identity` (DOUBLE; `100.0` for centroids),
`cigar` (VARCHAR; empty for centroids), `cigar_truncated`
(BOOLEAN; `true` if CIGAR was truncated past 4096 chars).

Single-threaded by construction (each new centroid must be indexed
before processing the next sequence). RNA `U` is auto-converted to
DNA `T`. Materializes all results before returning.
)DOC";

	RegisterDocumentedTableFunction(
	    loader, GetFunction(), description, {"input_table"},
	    {
	        "-- cluster_fast equivalent: longest first\n"
	        "CREATE TABLE sorted_seqs AS\n"
	        "  SELECT * FROM read_fastx('sequences.fasta')\n"
	        "  ORDER BY length(sequence1) DESC;\n"
	        "SELECT * FROM cluster_sequences_vsearch('sorted_seqs', id := 0.97);",
	        "-- cluster_size equivalent: by abundance\n"
	        "CREATE TABLE by_abundance AS\n"
	        "  SELECT read_id, sequence1, count(*) AS size\n"
	        "  FROM read_fastx('amplicons.fasta')\n"
	        "  GROUP BY read_id, sequence1\n"
	        "  ORDER BY size DESC;\n"
	        "SELECT * FROM cluster_sequences_vsearch('by_abundance', id := 0.97);",
	        "-- Count clusters\n"
	        "SELECT count(*) FROM cluster_sequences_vsearch('sorted_seqs', id := 0.97)\n"
	        "WHERE is_centroid;",
	        "-- Cluster sizes, sorted\n"
	        "SELECT centroid_id, count(*) AS size\n"
	        "FROM cluster_sequences_vsearch('sorted_seqs', id := 0.97)\n"
	        "GROUP BY centroid_id ORDER BY size DESC;",
	    },
	    /*alias_of=*/"", /*categories=*/{"microbiome"});
}

} // namespace duckdb
