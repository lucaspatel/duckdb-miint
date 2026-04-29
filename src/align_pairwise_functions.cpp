#include "align_pairwise_functions.hpp"

#include "WFA2Aligner.hpp"
#include "documented_function.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace duckdb {

// ---------------------------------------------------------------------------
// Shared bind data: stores method + penalty constants validated at bind time
// ---------------------------------------------------------------------------
struct AlignPairwiseBindData : public FunctionData {
	std::string method;
	int mismatch;
	int gap_open;
	int gap_extend;

	AlignPairwiseBindData(std::string method_p, int mismatch_p, int gap_open_p, int gap_extend_p)
	    : method(std::move(method_p)), mismatch(mismatch_p), gap_open(gap_open_p), gap_extend(gap_extend_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<AlignPairwiseBindData>(method, mismatch, gap_open, gap_extend);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<AlignPairwiseBindData>();
		return method == other.method && mismatch == other.mismatch && gap_open == other.gap_open &&
		       gap_extend == other.gap_extend;
	}

	// Validate method and penalties. Penalty validation is delegated to WFA2Aligner
	// constructor (single source of truth) with exception type translation.
	static void ValidatePenalties(const std::string &method, int mismatch, int gap_open, int gap_extend) {
		if (method != "wfa2") {
			throw InvalidInputException("Invalid method '%s'. Supported methods: 'wfa2'.", method);
		}
		try {
			miint::WFA2Aligner test(mismatch, gap_open, gap_extend);
		} catch (const std::invalid_argument &e) {
			throw InvalidInputException(e.what());
		}
	}

	// Extract and validate penalty parameters from 6-arg overload bind arguments
	static unique_ptr<AlignPairwiseBindData> FromArgs6(ClientContext &context,
	                                                   vector<unique_ptr<Expression>> &arguments) {
		for (idx_t i = 2; i < 6; i++) {
			if (!arguments[i]->IsFoldable()) {
				throw InvalidInputException(
				    "align_pairwise_* penalty parameters must be constant values, not column references");
			}
		}
		auto method = ExpressionExecutor::EvaluateScalar(context, *arguments[2]).GetValue<string>();
		auto mismatch = ExpressionExecutor::EvaluateScalar(context, *arguments[3]).GetValue<int>();
		auto gap_open = ExpressionExecutor::EvaluateScalar(context, *arguments[4]).GetValue<int>();
		auto gap_extend = ExpressionExecutor::EvaluateScalar(context, *arguments[5]).GetValue<int>();
		ValidatePenalties(method, mismatch, gap_open, gap_extend);
		return make_uniq<AlignPairwiseBindData>(method, mismatch, gap_open, gap_extend);
	}

	static unique_ptr<AlignPairwiseBindData> Defaults() {
		return make_uniq<AlignPairwiseBindData>("wfa2", 4, 6, 2);
	}
};

// ---------------------------------------------------------------------------
// Shared local state: per-thread WFA2Aligner reused across all rows
// ---------------------------------------------------------------------------
struct AlignPairwiseLocalState : public FunctionLocalState {
	miint::WFA2Aligner aligner;
	// Reusable buffers to avoid per-row heap allocations in the execute loop.
	// Pre-reserved for typical Illumina read length (150bp).
	std::string query_buf;
	std::string subject_buf;

	AlignPairwiseLocalState(int mismatch, int gap_open, int gap_extend) : aligner(mismatch, gap_open, gap_extend) {
		query_buf.reserve(256);
		subject_buf.reserve(256);
	}
};

static unique_ptr<FunctionLocalState>
AlignPairwiseInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr, FunctionData *bind_data) {
	auto &data = bind_data->Cast<AlignPairwiseBindData>();
	return make_uniq<AlignPairwiseLocalState>(data.mismatch, data.gap_open, data.gap_extend);
}

// ---------------------------------------------------------------------------
// Shared execute helpers
// ---------------------------------------------------------------------------

// Prepared input vectors for the execute loop
struct AlignInputVectors {
	UnifiedVectorFormat query_data;
	UnifiedVectorFormat subject_data;
	const string_t *query_ptr;
	const string_t *subject_ptr;
};

static AlignInputVectors PrepareInputs(DataChunk &args) {
	AlignInputVectors v;
	args.data[0].ToUnifiedFormat(args.size(), v.query_data);
	args.data[1].ToUnifiedFormat(args.size(), v.subject_data);
	v.query_ptr = UnifiedVectorFormat::GetData<string_t>(v.query_data);
	v.subject_ptr = UnifiedVectorFormat::GetData<string_t>(v.subject_data);
	return v;
}

// Populate reusable buffers from input vectors. Returns false if either input is NULL.
static bool GetAlignInput(AlignInputVectors &v, idx_t i, std::string &query_buf, std::string &subject_buf) {
	auto qi = v.query_data.sel->get_index(i);
	auto si = v.subject_data.sel->get_index(i);
	if (!v.query_data.validity.RowIsValid(qi) || !v.subject_data.validity.RowIsValid(si)) {
		return false;
	}
	query_buf.assign(v.query_ptr[qi].GetData(), v.query_ptr[qi].GetSize());
	subject_buf.assign(v.subject_ptr[si].GetData(), v.subject_ptr[si].GetSize());
	return true;
}

// 6-arg input types shared by all three functions
static vector<LogicalType> SixArgTypes() {
	return {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	        LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER};
}

// ---------------------------------------------------------------------------
// align_pairwise_score → INTEGER
// ---------------------------------------------------------------------------
static void AlignPairwiseScoreExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<AlignPairwiseLocalState>();
	auto inputs = PrepareInputs(args);
	auto result_data = FlatVector::GetData<int32_t>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < args.size(); i++) {
		if (!GetAlignInput(inputs, i, lstate.query_buf, lstate.subject_buf)) {
			result_validity.SetInvalid(i);
			continue;
		}
		auto score = lstate.aligner.align_score(lstate.query_buf, lstate.subject_buf);
		if (!score.has_value()) {
			result_validity.SetInvalid(i);
			continue;
		}
		result_data[i] = score.value();
	}
}

// Shared overloads for the three align_pairwise_* function sets. Each
// function set carries a 2-arg form (defaults) and a 6-arg form (explicit
// `mismatch / gap_open / gap_extend` penalties).
static const std::initializer_list<DocumentedOverload> kAlignPairwiseOverloads = {
    {{"query", "subject"}, {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR}},
    {{"query", "subject", "method", "mismatch", "gap_open", "gap_extend"},
     {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR, LogicalTypeId::INTEGER,
      LogicalTypeId::INTEGER, LogicalTypeId::INTEGER}},
};

static const std::string kAlignPairwiseShared = R"DOC(
Gap-affine pairwise sequence alignment, powered by
[WFA2-lib](https://github.com/smarco/WFA2-lib) (Wavefront Alignment
Algorithm). Three companion functions at increasing detail levels:

- [`align_pairwise_score`](../align_pairwise_score/) — score only
  (fastest).
- [`align_pairwise_cigar`](../align_pairwise_cigar/) — score + extended
  CIGAR (uses `=`/`X` rather than `M`).
- [`align_pairwise_full`](../align_pairwise_full/) — score + CIGAR +
  the two aligned sequences with `-` gap characters.

Each function has a 2-arg form (default penalties: `mismatch=4`,
`gap_open=6`, `gap_extend=2`) and a 6-arg form with explicit
penalties.

### 6-arg parameters

- `query` (VARCHAR), `subject` (VARCHAR).
- `method` (VARCHAR) — currently only `'wfa2'`.
- `mismatch` (INTEGER, > 0).
- `gap_open` (INTEGER, ≥ 0).
- `gap_extend` (INTEGER, > 0).

Penalty parameters must be constant values (not column references)
because they're consumed at bind time. NULL inputs produce NULL
output. Alignment failure (e.g., excessive divergence) also produces
NULL.
)DOC";

void AlignPairwiseScoreFunction::Register(ExtensionLoader &loader) {
	ScalarFunctionSet function_set("align_pairwise_score");

	ScalarFunction score_2arg("align_pairwise_score", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                          LogicalType::INTEGER, AlignPairwiseScoreExecute);
	score_2arg.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	score_2arg.bind = [](ClientContext &ctx, ScalarFunction &fn, vector<unique_ptr<Expression>> &args) {
		return unique_ptr<FunctionData>(AlignPairwiseBindData::Defaults().release());
	};
	score_2arg.init_local_state = AlignPairwiseInitLocalState;
	function_set.AddFunction(score_2arg);

	ScalarFunction score_6arg("align_pairwise_score", SixArgTypes(), LogicalType::INTEGER, AlignPairwiseScoreExecute);
	score_6arg.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	score_6arg.bind = [](ClientContext &ctx, ScalarFunction &fn, vector<unique_ptr<Expression>> &args) {
		return unique_ptr<FunctionData>(AlignPairwiseBindData::FromArgs6(ctx, args).release());
	};
	score_6arg.init_local_state = AlignPairwiseInitLocalState;
	function_set.AddFunction(score_6arg);

	static const std::string description = kAlignPairwiseShared + R"DOC(
Returns the alignment score as `INTEGER` (0 = identical, higher =
more divergent). Cheapest of the three since CIGAR / aligned
sequences are not built.
)DOC";

	RegisterDocumentedScalarSet(loader, std::move(function_set), description, kAlignPairwiseOverloads,
	                            {
	                                "-- Identical sequences\n"
	                                "SELECT align_pairwise_score('ACGT', 'ACGT');",
	                                "-- One mismatch (default penalty 4)\n"
	                                "SELECT align_pairwise_score('ACGT', 'ACAT');",
	                                "-- Custom penalties\n"
	                                "SELECT align_pairwise_score(query, subject, 'wfa2', 2, 6, 2)\n"
	                                "FROM sequence_pairs;",
	                            },
	                            /*alias_of=*/"", /*categories=*/{"pairwise"},
	                            /*executable_examples=*/
	                            {
	                                "SELECT align_pairwise_score('ACGT', 'ACGT');",
	                                "SELECT align_pairwise_score('ACGT', 'ACAT');",
	                            });
}

// ---------------------------------------------------------------------------
// align_pairwise_cigar → STRUCT(score INTEGER, cigar VARCHAR)
// ---------------------------------------------------------------------------
static LogicalType CigarReturnType() {
	return LogicalType::STRUCT({{"score", LogicalType::INTEGER}, {"cigar", LogicalType::VARCHAR}});
}

static void AlignPairwiseCigarExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<AlignPairwiseLocalState>();
	auto inputs = PrepareInputs(args);

	auto &entries = StructVector::GetEntries(result);
	auto score_data = FlatVector::GetData<int32_t>(*entries[0]);
	auto &cigar_vec = *entries[1];
	auto cigar_data = FlatVector::GetData<string_t>(cigar_vec);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < args.size(); i++) {
		if (!GetAlignInput(inputs, i, lstate.query_buf, lstate.subject_buf)) {
			result_validity.SetInvalid(i);
			continue;
		}
		auto cigar_result = lstate.aligner.align_cigar(lstate.query_buf, lstate.subject_buf);
		if (!cigar_result.has_value()) {
			result_validity.SetInvalid(i);
			continue;
		}
		score_data[i] = cigar_result->score;
		cigar_data[i] = StringVector::AddString(cigar_vec, cigar_result->cigar);
	}
}

void AlignPairwiseCigarFunction::Register(ExtensionLoader &loader) {
	ScalarFunctionSet function_set("align_pairwise_cigar");

	ScalarFunction cigar_2arg("align_pairwise_cigar", {LogicalType::VARCHAR, LogicalType::VARCHAR}, CigarReturnType(),
	                          AlignPairwiseCigarExecute);
	cigar_2arg.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	cigar_2arg.bind = [](ClientContext &ctx, ScalarFunction &fn, vector<unique_ptr<Expression>> &args) {
		fn.return_type = CigarReturnType();
		return unique_ptr<FunctionData>(AlignPairwiseBindData::Defaults().release());
	};
	cigar_2arg.init_local_state = AlignPairwiseInitLocalState;
	function_set.AddFunction(cigar_2arg);

	ScalarFunction cigar_6arg("align_pairwise_cigar", SixArgTypes(), CigarReturnType(), AlignPairwiseCigarExecute);
	cigar_6arg.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	cigar_6arg.bind = [](ClientContext &ctx, ScalarFunction &fn, vector<unique_ptr<Expression>> &args) {
		fn.return_type = CigarReturnType();
		return unique_ptr<FunctionData>(AlignPairwiseBindData::FromArgs6(ctx, args).release());
	};
	cigar_6arg.init_local_state = AlignPairwiseInitLocalState;
	function_set.AddFunction(cigar_6arg);

	static const std::string description = kAlignPairwiseShared + R"DOC(
Returns `STRUCT(score INTEGER, cigar VARCHAR)` — extended CIGAR uses
`=` for matches and `X` for mismatches (rather than `M`).
)DOC";

	RegisterDocumentedScalarSet(loader, std::move(function_set), description, kAlignPairwiseOverloads,
	                            {
	                                "-- Just the CIGAR string\n"
	                                "SELECT (align_pairwise_cigar('ACGT', 'ACAT')).cigar;",
	                                "-- Score + CIGAR over a pairs table\n"
	                                "SELECT name,\n"
	                                "       (align_pairwise_cigar(query_seq, ref_seq)).score,\n"
	                                "       (align_pairwise_cigar(query_seq, ref_seq)).cigar\n"
	                                "FROM sequence_pairs;",
	                            },
	                            /*alias_of=*/"", /*categories=*/{"pairwise"},
	                            /*executable_examples=*/
	                            {
	                                "SELECT (align_pairwise_cigar('ACGT', 'ACAT')).cigar;",
	                            });
}

// ---------------------------------------------------------------------------
// align_pairwise_full → STRUCT(score INTEGER, cigar VARCHAR,
//                              query_aligned VARCHAR, subject_aligned VARCHAR)
// ---------------------------------------------------------------------------
static LogicalType FullReturnType() {
	return LogicalType::STRUCT({{"score", LogicalType::INTEGER},
	                            {"cigar", LogicalType::VARCHAR},
	                            {"query_aligned", LogicalType::VARCHAR},
	                            {"subject_aligned", LogicalType::VARCHAR}});
}

static void AlignPairwiseFullExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<AlignPairwiseLocalState>();
	auto inputs = PrepareInputs(args);

	auto &entries = StructVector::GetEntries(result);
	auto score_data = FlatVector::GetData<int32_t>(*entries[0]);
	auto &cigar_vec = *entries[1];
	auto &query_aligned_vec = *entries[2];
	auto &subject_aligned_vec = *entries[3];
	auto cigar_data = FlatVector::GetData<string_t>(cigar_vec);
	auto query_aligned_data = FlatVector::GetData<string_t>(query_aligned_vec);
	auto subject_aligned_data = FlatVector::GetData<string_t>(subject_aligned_vec);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < args.size(); i++) {
		if (!GetAlignInput(inputs, i, lstate.query_buf, lstate.subject_buf)) {
			result_validity.SetInvalid(i);
			continue;
		}
		auto full_result = lstate.aligner.align_full(lstate.query_buf, lstate.subject_buf);
		if (!full_result.has_value()) {
			result_validity.SetInvalid(i);
			continue;
		}
		score_data[i] = full_result->score;
		cigar_data[i] = StringVector::AddString(cigar_vec, full_result->cigar);
		query_aligned_data[i] = StringVector::AddString(query_aligned_vec, full_result->query_aligned);
		subject_aligned_data[i] = StringVector::AddString(subject_aligned_vec, full_result->subject_aligned);
	}
}

void AlignPairwiseFullFunction::Register(ExtensionLoader &loader) {
	ScalarFunctionSet function_set("align_pairwise_full");

	ScalarFunction full_2arg("align_pairwise_full", {LogicalType::VARCHAR, LogicalType::VARCHAR}, FullReturnType(),
	                         AlignPairwiseFullExecute);
	full_2arg.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	full_2arg.bind = [](ClientContext &ctx, ScalarFunction &fn, vector<unique_ptr<Expression>> &args) {
		fn.return_type = FullReturnType();
		return unique_ptr<FunctionData>(AlignPairwiseBindData::Defaults().release());
	};
	full_2arg.init_local_state = AlignPairwiseInitLocalState;
	function_set.AddFunction(full_2arg);

	ScalarFunction full_6arg("align_pairwise_full", SixArgTypes(), FullReturnType(), AlignPairwiseFullExecute);
	full_6arg.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	full_6arg.bind = [](ClientContext &ctx, ScalarFunction &fn, vector<unique_ptr<Expression>> &args) {
		fn.return_type = FullReturnType();
		return unique_ptr<FunctionData>(AlignPairwiseBindData::FromArgs6(ctx, args).release());
	};
	full_6arg.init_local_state = AlignPairwiseInitLocalState;
	function_set.AddFunction(full_6arg);

	static const std::string description = kAlignPairwiseShared + R"DOC(
Returns `STRUCT(score INTEGER, cigar VARCHAR, query_aligned VARCHAR,
subject_aligned VARCHAR)` — the two `*_aligned` strings have `-`
inserted at gap positions, so the two columns are equal length.
)DOC";

	RegisterDocumentedScalarSet(loader, std::move(function_set), description, kAlignPairwiseOverloads,
	                            {
	                                "-- Aligned sequences with gaps shown\n"
	                                "SELECT (align_pairwise_full('ACGT', 'AGT')).query_aligned,\n"
	                                "       (align_pairwise_full('ACGT', 'AGT')).subject_aligned;",
	                                "-- Custom-penalty alignment over a table of amplicon pairs\n"
	                                "SELECT (align_pairwise_full(seq_a, seq_b, 'wfa2', 2, 4, 1)).query_aligned\n"
	                                "FROM amplicon_pairs;",
	                            },
	                            /*alias_of=*/"", /*categories=*/{"pairwise"},
	                            /*executable_examples=*/
	                            {
	                                "SELECT (align_pairwise_full('ACGT', 'AGT')).query_aligned;",
	                            });
}

} // namespace duckdb
