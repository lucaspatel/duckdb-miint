#include "mask_dust_function.hpp"

#include "documented_function.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "mask.h"

namespace duckdb {

struct DustSoftMaskOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, Vector &result) {
		auto len = input.GetSize();
		if (len == 0) {
			return StringVector::AddString(result, "", 0);
		}
		std::string tmp(input.GetData(), len);
		dust_single(tmp.data(), static_cast<int>(len), false);
		return StringVector::AddString(result, tmp);
	}
};

static void MaskDustSoftFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::ExecuteString<string_t, string_t, DustSoftMaskOperator>(args.data[0], result, args.size());
}

static void MaskDustHardmaskFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::ExecuteWithNulls<string_t, bool, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t input, bool hardmask, ValidityMask &mask, idx_t idx) -> string_t {
		    auto len = input.GetSize();
		    if (len == 0) {
			    return StringVector::AddString(result, "", 0);
		    }
		    std::string tmp(input.GetData(), len);
		    dust_single(tmp.data(), static_cast<int>(len), hardmask);
		    return StringVector::AddString(result, tmp);
	    });
}

void MaskDustFunction::Register(ExtensionLoader &loader) {
	ScalarFunctionSet set("mask_dust");
	// 1-arg: soft-mask (lowercase)
	set.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::VARCHAR, MaskDustSoftFunction));
	// 2-arg: mask with hardmask boolean
	set.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::BOOLEAN}, LogicalType::VARCHAR, MaskDustHardmaskFunction));

	static const std::string description = R"DOC(
DUST low-complexity masking, powered by the
[vsearch](https://github.com/torognes/vsearch) library
(Rognes et al. 2016, *PeerJ* 4:e2584). Identifies low-complexity regions
(homopolymers, dinucleotide repeats, etc.) and either soft-masks them
(lowercase, default) or hard-masks them (replace with `N`).

High-complexity sequences are returned unchanged. NULL → NULL; empty
string → empty string.
)DOC";
	RegisterDocumentedScalarSet(loader, set, description,
	                            {
	                                {{"sequence"}, {LogicalTypeId::VARCHAR}},
	                                {{"sequence", "hardmask"}, {LogicalTypeId::VARCHAR, LogicalTypeId::BOOLEAN}},
	                            },
	                            {
	                                "-- Soft-mask\nSELECT mask_dust('AAAAAAAAAAAACCCCCCCC');",
	                                "-- Hard-mask\nSELECT mask_dust('AAAAAAAAAAAACCCCCCCC', true);",
	                                "-- Mask sequences in a table\n"
	                                "SELECT read_id, mask_dust(sequence1) AS masked_seq\n"
	                                "FROM read_fastx('sequences.fasta');",
	                            },
	                            /*alias_of=*/"", /*categories=*/{"sequence-tools"},
	                            /*executable_examples=*/
	                            {
	                                "SELECT mask_dust('AAAAAAAAAAAACCCCCCCC');",
	                                "SELECT mask_dust('AAAAAAAAAAAACCCCCCCC', true);",
	                            });
}

} // namespace duckdb
