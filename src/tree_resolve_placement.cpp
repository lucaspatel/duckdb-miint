#include "tree_resolve_placement.hpp"
#include "documented_function.hpp"
#include "placement_table_reader.hpp"
#include "tree_table_reader.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

TreeResolvePlacementTableFunction::Data::Data(std::string tree_table, std::string placements_table)
    : tree_table_name(std::move(tree_table)), placements_table_name(std::move(placements_table)) {
	ReadNewickTableFunction::GetSchema(names, types, false);
}

unique_ptr<FunctionData> TreeResolvePlacementTableFunction::Bind(ClientContext &context, TableFunctionBindInput &input,
                                                                 vector<LogicalType> &return_types,
                                                                 vector<std::string> &names) {
	auto tree_table_name = input.inputs[0].ToString();
	auto placements_table_name = input.inputs[1].ToString();

	// Validate schemas at bind time for early error detection
	ValidateTreeTableSchema(context, tree_table_name);
	ValidatePlacementTableSchema(context, placements_table_name);

	auto data = duckdb::make_uniq<Data>(std::move(tree_table_name), std::move(placements_table_name));

	for (const auto &name : data->names) {
		names.emplace_back(name);
	}
	for (const auto &type : data->types) {
		return_types.emplace_back(type);
	}

	return data;
}

unique_ptr<GlobalTableFunctionState> TreeResolvePlacementTableFunction::InitGlobal(ClientContext &context,
                                                                                   TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<Data>();
	auto gstate = duckdb::make_uniq<GlobalState>();

	// Read tree data and placements
	auto node_inputs = ReadTreeTable(context, bind_data.tree_table_name);
	auto placements = ReadPlacementTable(context, bind_data.placements_table_name);

	// Build tree from node data
	miint::NewickTree tree;
	try {
		tree = miint::NewickTree::build(node_inputs);
	} catch (const std::exception &e) {
		throw InvalidInputException("Failed to build tree from '%s': %s", bind_data.tree_table_name, e.what());
	}

	// Resolve placements
	try {
		tree.insert_fully_resolved(placements);
	} catch (const std::runtime_error &e) {
		throw InvalidInputException("Failed to resolve placements: %s", e.what());
	}

	// Convert to rows
	gstate->rows = ReadNewickTableFunction::TreeToRows(tree);

	return gstate;
}

void TreeResolvePlacementTableFunction::Execute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &global_state = data_p.global_state->Cast<GlobalState>();

	if (global_state.current_row_idx >= global_state.rows.size()) {
		output.SetCardinality(0);
		return;
	}

	size_t rows_to_output =
	    std::min<size_t>(STANDARD_VECTOR_SIZE, global_state.rows.size() - global_state.current_row_idx);

	ReadNewickTableFunction::EmitNodeRows(global_state.rows, global_state.current_row_idx, rows_to_output, output, 0,
	                                      false, "");

	global_state.current_row_idx += rows_to_output;
	output.SetCardinality(rows_to_output);
}

TableFunction TreeResolvePlacementTableFunction::GetFunction() {
	return TableFunction("tree_resolve_placement", {LogicalType::VARCHAR, LogicalType::VARCHAR}, Execute, Bind,
	                     InitGlobal);
}

void TreeResolvePlacementTableFunction::Register(ExtensionLoader &loader) {
	static const std::string description = R"DOC(
Resolve phylogenetic placements into a reference tree, returning a
fully resolved tree with placed fragments as new tips. Exposes the
`insert_fully_resolved` algorithm as a SQL table function.

Each placement creates two new nodes: an internal node that splits
the target edge, and a fragment tip. Placements are deduplicated by
`fragment_id` (highest `like_weight_ratio`, then lowest
`pendant_length`); multiple placements on the same edge are sorted
by `distal_length` and inserted as a chain. Original tip-to-tip
distances are preserved.

### Inputs

- `tree_table` (VARCHAR) — table/view in
  [`read_newick`](../read_newick/) schema. Requires `node_index` and
  `parent_index`; `name`, `branch_length`, `edge_id` are optional.
- `placements_table` (VARCHAR) — table/view with `fragment_id`,
  `edge_id`, `like_weight_ratio`, `distal_length`, `pendant_length`.

### Output schema

Same as `read_newick` (without `filepath`): `node_index`, `name`
(placed fragments use their `fragment_id`), `branch_length`,
`edge_id` (NULL for newly created nodes), `parent_index`, `is_tip`.
UNION-ALL-compatible with `read_newick` output.
)DOC";

	RegisterDocumentedTableFunction(
	    loader, GetFunction(), description, {"tree_table", "placements_table"},
	    {
	        "-- Build a tree and placements, then resolve\n"
	        "CREATE TABLE ref_tree AS SELECT * FROM read_newick('reference.nwk');\n"
	        "CREATE TABLE placements AS SELECT * FROM (VALUES\n"
	        "  ('seq1', 0::BIGINT, 0.95::DOUBLE, 0.05::DOUBLE, 0.001::DOUBLE),\n"
	        "  ('seq2', 1::BIGINT, 0.80::DOUBLE, 0.10::DOUBLE, 0.002::DOUBLE)\n"
	        ") AS t(fragment_id, edge_id, like_weight_ratio, distal_length, pendant_length);\n"
	        "SELECT * FROM tree_resolve_placement('ref_tree', 'placements');",
	        "-- Full jplace workflow: tree + placements both extracted from a .jplace\n"
	        "CREATE TABLE jplace_tree AS SELECT * FROM read_jplace_newick('results.jplace');\n"
	        "CREATE TABLE jplace_placements AS\n"
	        "  SELECT fragment AS fragment_id, edge_num::BIGINT AS edge_id,\n"
	        "         like_weight_ratio, distal_length, pendant_length\n"
	        "  FROM read_jplace('results.jplace');\n"
	        "SELECT name FROM tree_resolve_placement('jplace_tree', 'jplace_placements')\n"
	        "WHERE is_tip = true ORDER BY name;",
	        "-- Write the resolved tree out as Newick\n"
	        "COPY (\n"
	        "  SELECT node_index, name, branch_length, edge_id, parent_index\n"
	        "  FROM tree_resolve_placement('ref_tree', 'placements')\n"
	        ") TO 'resolved.nwk' (FORMAT NEWICK);",
	    },
	    /*alias_of=*/"", /*categories=*/{"phylogeny"});
}

} // namespace duckdb
