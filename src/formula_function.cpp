#include "formula_function.hpp"

#include "documented_function.hpp"
#include "formula_parser.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <string>

namespace duckdb {

static void FormulaScalarFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(), [](string_t input) {
		try {
			return miint::ParseFormula(input.GetString());
		} catch (const std::runtime_error &e) {
			throw InvalidInputException(e.what());
		}
	});
}

void FormulaFunction::Register(ExtensionLoader &loader) {
	RegisterDocumentedScalar(
	    loader,
	    ScalarFunction("formula", {LogicalType::VARCHAR}, LogicalType::DOUBLE, FormulaScalarFunction),
	    "Compute the monoisotopic mass (Daltons) of a chemical formula like `'H2O'`, `'C6H12O6'`, or `'Fe'`.",
	    {"formula_string"},
	    {
	        "SELECT formula('H2O');       -- 18.010565",
	        "SELECT formula('C6H12O6');   -- 180.063388",
	        "SELECT formula('Fe');        -- 55.934936",
	    },
	    /*alias_of=*/"", /*categories=*/{"mass-spec-analysis"},
	    /*executable_examples=*/
	    {
	        "SELECT formula('H2O');",
	        "SELECT formula('C6H12O6');",
	    });
}

} // namespace duckdb
