#include "compress_intervals.hpp"
#include "documented_function.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

namespace duckdb {

struct IntervalState {
	miint::IntervalCompressor *compressor;

	IntervalState() : compressor(nullptr) {
	}

	void Compress() {
		compressor->Compress();
	}

	void Add(int64_t start, int64_t stop) {
		compressor->Add(start, stop);
	}

	bool Empty() const {
		return compressor->Empty();
	}

	size_t Size() const {
		return compressor->Size();
	}

	const std::vector<int64_t> &Starts() const {
		return compressor->starts;
	}

	const std::vector<int64_t> &Stops() const {
		return compressor->stops;
	}
};

struct CompressIntervalsBindData : public FunctionData {
	explicit CompressIntervalsBindData() {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<CompressIntervalsBindData>();
	}

	bool Equals(const FunctionData &other) const override {
		return true;
	}
};

struct CompressIntervalsOperation {
	template <class STATE>
	static void Initialize(STATE &state) {
		state.compressor = new miint::IntervalCompressor();
	}

	template <class STATE>
	static void Destroy(STATE &state, AggregateInputData &aggr_input_data) {
		delete state.compressor;
	}

	static void Operation(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count, Vector &states,
	                      idx_t count) {
		auto &start_vector = inputs[0];
		auto &stop_vector = inputs[1];

		UnifiedVectorFormat start_data;
		UnifiedVectorFormat stop_data;
		start_vector.ToUnifiedFormat(count, start_data);
		stop_vector.ToUnifiedFormat(count, stop_data);

		auto start_ptr = UnifiedVectorFormat::GetData<int64_t>(start_data);
		auto stop_ptr = UnifiedVectorFormat::GetData<int64_t>(stop_data);

		UnifiedVectorFormat state_data;
		states.ToUnifiedFormat(count, state_data);
		auto state_ptr = UnifiedVectorFormat::GetData<IntervalState *>(state_data);

		for (idx_t i = 0; i < count; i++) {
			auto state_idx = state_data.sel->get_index(i);
			auto start_idx = start_data.sel->get_index(i);
			auto stop_idx = stop_data.sel->get_index(i);

			if (!start_data.validity.RowIsValid(start_idx) || !stop_data.validity.RowIsValid(stop_idx)) {
				continue;
			}

			auto *state = state_ptr[state_idx];
			state->Add(start_ptr[start_idx], stop_ptr[stop_idx]);
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &aggr_input_data) {
		for (idx_t i = 0; i < source.Size(); i++) {
			target.Add(source.Starts()[i], source.Stops()[i]);
		}
		// Compress after combining to avoid memory bloat in parallel execution
		target.Compress();
	}

	static void Finalize(Vector &state_vector, AggregateInputData &aggr_input_data, Vector &result, idx_t count,
	                     idx_t offset) {
		UnifiedVectorFormat state_data;
		state_vector.ToUnifiedFormat(count, state_data);
		auto states = UnifiedVectorFormat::GetData<IntervalState *>(state_data);

		auto &result_validity = FlatVector::Validity(result);
		auto result_data = FlatVector::GetData<list_entry_t>(result);

		for (idx_t i = 0; i < count; i++) {
			auto state_idx = state_data.sel->get_index(i);
			auto &state = *states[state_idx];

			state.Compress();

			if (state.Empty()) {
				result_validity.SetInvalid(i + offset);
				continue;
			}

			auto &list_entry = ListVector::GetEntry(result);
			auto list_offset = ListVector::GetListSize(result);
			ListVector::Reserve(result, list_offset + state.Size());

			auto &struct_children = StructVector::GetEntries(list_entry);
			auto &start_child = struct_children[0];
			auto &stop_child = struct_children[1];

			auto start_ptr = FlatVector::GetData<int64_t>(*start_child);
			auto stop_ptr = FlatVector::GetData<int64_t>(*stop_child);

			for (idx_t j = 0; j < state.Size(); j++) {
				start_ptr[list_offset + j] = state.Starts()[j];
				stop_ptr[list_offset + j] = state.Stops()[j];
			}

			ListVector::SetListSize(result, list_offset + state.Size());

			result_data[i + offset].offset = list_offset;
			result_data[i + offset].length = state.Size();
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

void CompressIntervalsFunction::Register(ExtensionLoader &loader) {
	auto fun = AggregateFunction(
	    "compress_intervals", {LogicalType::BIGINT, LogicalType::BIGINT},
	    LogicalType::LIST(LogicalType::STRUCT({{"start", LogicalType::BIGINT}, {"stop", LogicalType::BIGINT}})),
	    AggregateFunction::StateSize<IntervalState>,
	    AggregateFunction::StateInitialize<IntervalState, CompressIntervalsOperation>,
	    CompressIntervalsOperation::Operation,
	    AggregateFunction::StateCombine<IntervalState, CompressIntervalsOperation>,
	    CompressIntervalsOperation::Finalize, nullptr, nullptr,
	    AggregateFunction::StateDestroy<IntervalState, CompressIntervalsOperation>);

	static const std::string description = R"DOC(
Aggregate that merges overlapping or touching `(start, stop)` interval
pairs into a minimal set of non-overlapping intervals, sorted by start.
Two intervals merge when they overlap or touch (`stop1 == start2`).

Returns a `LIST<STRUCT(start BIGINT, stop BIGINT)>`. Empty group → NULL.
Thread-safe: each thread maintains its own state, merged at finalization.
Auto-compresses state when accumulating >1M intervals to prevent memory
bloat. Algorithm is O(n log n) — sort by start, single-pass merge.
)DOC";
	RegisterDocumentedAggregate(
	    loader, fun, description, {"start", "stop"},
	    {
	        "-- Coverage regions per reference\n"
	        "SELECT reference, compress_intervals(position, stop_position) AS coverage\n"
	        "FROM read_alignments('alignments.bam') GROUP BY reference;",

	        "-- Total covered bases per reference\n"
	        "SELECT reference, SUM(c.stop - c.start) AS total_coverage\n"
	        "FROM (SELECT reference, UNNEST(compress_intervals(position, stop_position)) AS c\n"
	        "      FROM read_alignments('alignments.bam') GROUP BY reference);",

	        "-- Merge feature intervals from different sources\n"
	        "SELECT ref, compress_intervals(start, stop) AS merged_regions\n"
	        "FROM features GROUP BY ref;",
	    },
	    /*alias_of=*/"", /*categories=*/{"intervals"},
	    /*executable_examples=*/
	    {
	        "SELECT compress_intervals(s, e) FROM (VALUES (10, 20), (15, 25), (30, 40)) AS t(s, e);",
	    });
}

} // namespace duckdb
