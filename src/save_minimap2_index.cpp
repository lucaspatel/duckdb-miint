#include "save_minimap2_index.hpp"
#include "documented_function.hpp"
#include "sequence_table_reader.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

unique_ptr<FunctionData> SaveMinimap2IndexTableFunction::Bind(ClientContext &context, TableFunctionBindInput &input,
                                                              vector<LogicalType> &return_types,
                                                              vector<std::string> &names) {
	auto data = make_uniq<Data>();

	// Required positional parameters: subject_table, output_path
	if (input.inputs.size() < 2) {
		throw BinderException("save_minimap2_index requires subject_table and output_path parameters");
	}

	data->subject_table = input.inputs[0].ToString();
	data->output_path = input.inputs[1].ToString();

	// Validate subject table exists and has correct schema
	ValidateSequenceTableSchema(context, data->subject_table);

	// Parse optional named parameters (same as align_minimap2)
	auto preset_param = input.named_parameters.find("preset");
	if (preset_param != input.named_parameters.end() && !preset_param->second.IsNull()) {
		data->config.preset = preset_param->second.ToString();
	}

	auto k_param = input.named_parameters.find("k");
	if (k_param != input.named_parameters.end() && !k_param->second.IsNull()) {
		data->config.k = k_param->second.GetValue<int32_t>();
	}

	auto w_param = input.named_parameters.find("w");
	if (w_param != input.named_parameters.end() && !w_param->second.IsNull()) {
		data->config.w = w_param->second.GetValue<int32_t>();
	}

	auto eqx_param = input.named_parameters.find("eqx");
	if (eqx_param != input.named_parameters.end() && !eqx_param->second.IsNull()) {
		data->config.eqx = eqx_param->second.GetValue<bool>();
	}

	// Load subjects at bind time
	data->subjects = ReadSubjectTable(context, data->subject_table);

	if (data->subjects.empty()) {
		throw BinderException("Subject table '%s' is empty. Cannot save index for empty subject set.",
		                      data->subject_table);
	}

	// Set output schema
	for (const auto &name : data->names) {
		names.emplace_back(name);
	}
	for (const auto &type : data->types) {
		return_types.emplace_back(type);
	}

	return data;
}

unique_ptr<GlobalTableFunctionState> SaveMinimap2IndexTableFunction::InitGlobal(ClientContext &context,
                                                                                TableFunctionInitInput &input) {
	auto &data = input.bind_data->Cast<Data>();
	auto gstate = make_uniq<GlobalState>();

	// Create aligner with config
	auto aligner = std::make_unique<miint::Minimap2Aligner>(data.config);

	// Build index from subjects
	try {
		aligner->build_index(data.subjects);
	} catch (const std::exception &e) {
		throw IOException("Failed to build minimap2 index: %s", e.what());
	}

	// Save index to file
	try {
		aligner->save_index(data.output_path);
	} catch (const std::exception &e) {
		throw IOException("Failed to save minimap2 index to '%s': %s", data.output_path, e.what());
	}

	return gstate;
}

unique_ptr<LocalTableFunctionState> SaveMinimap2IndexTableFunction::InitLocal(ExecutionContext &context,
                                                                              TableFunctionInitInput &input,
                                                                              GlobalTableFunctionState *global_state) {
	return make_uniq<LocalState>();
}

void SaveMinimap2IndexTableFunction::Execute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<Data>();
	auto &global_state = data_p.global_state->Cast<GlobalState>();

	// Return single row with success information
	if (global_state.done) {
		output.SetCardinality(0);
		return;
	}

	// Set output values
	output.data[0].SetValue(0, Value::BOOLEAN(true));                                           // success
	output.data[1].SetValue(0, Value(bind_data.output_path));                                   // index_path
	output.data[2].SetValue(0, Value::BIGINT(static_cast<int64_t>(bind_data.subjects.size()))); // num_subjects

	output.SetCardinality(1);
	global_state.done = true;
}

TableFunction SaveMinimap2IndexTableFunction::GetFunction() {
	auto tf = TableFunction("save_minimap2_index", {LogicalType::VARCHAR, LogicalType::VARCHAR}, Execute, Bind,
	                        InitGlobal, InitLocal);

	// Named parameters (same options as align_minimap2 for index building)
	tf.named_parameters["preset"] = LogicalType::VARCHAR;
	tf.named_parameters["k"] = LogicalType::INTEGER;
	tf.named_parameters["w"] = LogicalType::INTEGER;
	tf.named_parameters["eqx"] = LogicalType::BOOLEAN;

	return tf;
}

void SaveMinimap2IndexTableFunction::Register(ExtensionLoader &loader) {
	static const std::string description = R"DOC(
Build a minimap2 index from a DuckDB table/view of reference sequences
and write it to disk as a `.mmi` file. Pair with
[`align_minimap2(..., index_path='file.mmi')`](../align_minimap2/) to
amortize indexing across many alignment runs (10–30× faster than
rebuilding from FASTA each time).

Returns one row: `(success BOOLEAN, index_path VARCHAR, num_subjects BIGINT)`.

### Named parameters

- `preset` (VARCHAR, default `'sr'`) — minimap2 preset; same options
  as `align_minimap2`.
- `k` (INTEGER) — k-mer size override.
- `w` (INTEGER) — minimizer window size override.
- `eqx` (BOOLEAN, default `true`) — use `=`/`X` CIGAR ops instead of `M`.

The subject table must have a `read_fastx`-compatible schema
(`read_id`, `sequence1`) and cannot contain paired-end data.
)DOC";

	RegisterDocumentedTableFunction(
	    loader, GetFunction(), description, {"subject_table", "output_path"},
	    {
	        "-- Build a short-read index from a reference FASTA, then reuse it\n"
	        "CREATE TABLE refs AS SELECT * FROM read_fastx('WoLr2_db.fna');\n"
	        "SELECT * FROM save_minimap2_index('refs', 'wolr2.mmi');",
	        "-- Build separate indexes for short-read vs long-read presets\n"
	        "SELECT * FROM save_minimap2_index('refs', 'wolr2_sr.mmi', preset='sr');\n"
	        "SELECT * FROM save_minimap2_index('refs', 'wolr2_ont.mmi', preset='map-ont');",
	        "-- Inspect the result row\n"
	        "SELECT success, index_path, num_subjects\n"
	        "FROM save_minimap2_index('refs', 'my_index.mmi', preset='sr');",
	        "-- Build, then align using the saved index (10-30x faster for large refs)\n"
	        "CREATE TABLE reads AS SELECT * FROM read_fastx('metagenome.fastq');\n"
	        "SELECT * FROM save_minimap2_index('refs', 'wolr2.mmi');\n"
	        "SELECT * FROM align_minimap2('reads', index_path='wolr2.mmi', max_secondary=0);",
	    },
	    /*alias_of=*/"", /*categories=*/{"alignment-tools"});
}

} // namespace duckdb
