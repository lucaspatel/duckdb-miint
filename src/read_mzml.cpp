#include "MzMLReader.hpp"
#include "documented_function.hpp"
#include "remote_file_helper.hpp"
#include "table_function_common.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include <read_mzml.hpp>

namespace duckdb {

unique_ptr<FunctionData> ReadMzMLTableFunction::Bind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<duckdb::LogicalType> &return_types,
                                                     vector<std::string> &names) {
	FileSystem &fs = FileSystem::GetFileSystem(context);

	std::vector<std::string> file_paths;

	if (input.inputs[0].type().id() == LogicalTypeId::VARCHAR) {
		auto result = ExpandGlobPatternWithInfo(fs, context, input.inputs[0].ToString());
		file_paths = std::move(result.paths);
	} else if (input.inputs[0].type().id() == LogicalTypeId::LIST) {
		auto &list_children = ListValue::GetChildren(input.inputs[0]);
		for (const auto &child : list_children) {
			file_paths.push_back(child.ToString());
		}
		if (file_paths.empty()) {
			throw InvalidInputException("read_mzml: at least one file path must be provided");
		}
	} else {
		throw InvalidInputException("read_mzml: first argument must be VARCHAR or VARCHAR[]");
	}

	for (const auto &path : file_paths) {
		if (IsStdinPath(path)) {
			throw InvalidInputException("read_mzml: stdin is not supported");
		}
	}

	for (const auto &path : file_paths) {
		if (!miint::RemoteFileHelper::IsRemotePath(path) && !fs.FileExists(path)) {
			throw IOException("File not found: " + path);
		}
	}

	bool include_filepath = ParseIncludeFilepathParameter(input.named_parameters);

	auto data = duckdb::make_uniq<Data>(file_paths, include_filepath);
	for (auto &name : data->names) {
		names.emplace_back(name);
	}
	for (auto &type : data->types) {
		return_types.emplace_back(type);
	}
	return data;
}

unique_ptr<GlobalTableFunctionState> ReadMzMLTableFunction::InitGlobal(ClientContext &context,
                                                                       TableFunctionInitInput &input) {
	auto &data = input.bind_data->Cast<Data>();
	auto &fs = FileSystem::GetFileSystem(context);
	return duckdb::make_uniq<GlobalState>(data.file_paths, fs);
}

unique_ptr<LocalTableFunctionState> ReadMzMLTableFunction::InitLocal(ExecutionContext &context,
                                                                     TableFunctionInitInput &input,
                                                                     GlobalTableFunctionState *global_state) {
	return duckdb::make_uniq<LocalState>();
}

void ReadMzMLTableFunction::Execute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<Data>();
	auto &global_state = data_p.global_state->Cast<GlobalState>();
	auto &local_state = data_p.local_state->Cast<LocalState>();

	miint::MzMLSpectrumBatch batch;
	std::string current_filepath;

	while (true) {
		if (!local_state.has_file) {
			lock_guard<mutex> read_lock(global_state.lock);

			if (global_state.next_file_idx >= global_state.filepaths.size()) {
				output.SetCardinality(0);
				return;
			}

			local_state.current_file_idx = global_state.next_file_idx;
			global_state.next_file_idx++;
			local_state.has_file = true;
		}

		// Safe without lock: each thread claims an exclusive file index via next_file_idx++
		// under the lock above, so no two threads access the same reader slot.
		if (!global_state.readers[local_state.current_file_idx]) {
			global_state.readers[local_state.current_file_idx] = std::make_unique<miint::MzMLReader>(
			    global_state.fs, global_state.filepaths[local_state.current_file_idx]);
		}

		batch = global_state.readers[local_state.current_file_idx]->read_spectra(STANDARD_VECTOR_SIZE);
		current_filepath = global_state.filepaths[local_state.current_file_idx];

		if (batch.empty()) {
			local_state.has_file = false;
			continue;
		}

		break;
	}

	PopulateSpectrumBatchOutput(output, batch, bind_data.include_filepath, current_filepath);
}

TableFunction ReadMzMLTableFunction::GetFunction() {
	auto tf = TableFunction("read_mzml", {LogicalType::ANY}, Execute, Bind, InitGlobal, InitLocal);
	tf.named_parameters["include_filepath"] = LogicalType::BOOLEAN;
	return tf;
}

void ReadMzMLTableFunction::Register(ExtensionLoader &loader) {
	static const std::string description = R"DOC(
Read mzML mass-spectrometry files into a relation with one row per
spectrum (27 columns: identifiers, MS-level metadata, precursor info,
peak arrays). Schema is `UNION ALL`-compatible with
[`read_mzxml`](../read_mzxml/) so mzML and mzXML can be combined.

Stdin not supported (mzML requires file seeking). Supports zlib-
compressed and uncompressed binary arrays at 32- or 64-bit precision.
For chromatograms (TIC/BPC/SRM/SIC) use
[`read_mzml_chromatograms`](../read_mzml_chromatograms/).

### Named parameters

| Name | Type | Default | Description |
|---|---|---|---|
| `include_filepath` | BOOLEAN | `false` | Add a `filepath` column. |

### Output schema (27 columns)

`spectrum_index`, `spectrum_id`, `scan_number`, `ms_level`,
`retention_time`, `spectrum_type`, `polarity`, `base_peak_mz`,
`base_peak_intensity`, `total_ion_current`, `lowest_mz`, `highest_mz`,
`default_array_length`, `precursor_mz`, `precursor_charge`,
`precursor_intensity`, `isolation_window_target`,
`isolation_window_lower`, `isolation_window_upper`, `activation_method`,
`collision_energy`, `mz_array` (DOUBLE[]), `intensity_array` (DOUBLE[]),
`filter_string`, `scan_window_lower`, `scan_window_upper`,
`ms1_scan_index`.
)DOC";
	RegisterDocumentedTableFunction(
	    loader, GetFunction(), description, {"filename"},
	    {
	        "-- Spectrum counts by MS level\n"
	        "SELECT ms_level, COUNT(*) FROM read_mzml('sample.mzML') GROUP BY ms_level;",
	        "-- MS2 spectra with precursor info\n"
	        "SELECT scan_number, precursor_mz, precursor_charge, activation_method\n"
	        "FROM read_mzml('sample.mzML') WHERE ms_level = 2;",
	        "-- Per-peak unnest for peak-level analysis\n"
	        "SELECT spectrum_index, ms_level,\n"
	        "       UNNEST(mz_array) AS mz, UNNEST(intensity_array) AS intensity\n"
	        "FROM read_mzml('sample.mzML');",
	        "-- Multi-file glob with filepath\n"
	        "SELECT * FROM read_mzml('data/mzml/*.mzML', include_filepath=true);",
	    },
	    /*alias_of=*/"", /*categories=*/{"mzml-io"});
}
} // namespace duckdb
