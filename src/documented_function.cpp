#include "documented_function.hpp"

#include "duckdb.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <utility>

namespace duckdb {

namespace {

// In-process registry of (function_name, executable SQL) pairs collected
// from RegisterDocumented*() calls. Read by GetDoctestRegistry() (used by
// the doctest test harness) and by the miint_doctest_examples() table
// function (used by the docs build's test-file generator).
std::vector<DoctestEntry> &MutableRegistry() {
	static std::vector<DoctestEntry> registry;
	return registry;
}

void AppendDoctests(const std::string &function_name, const std::vector<std::string> &executable_examples) {
	auto &registry = MutableRegistry();
	for (const auto &sql : executable_examples) {
		registry.push_back({function_name, sql});
	}
}

// Build a FunctionDescription from the user-supplied prose and the function's
// own argument types. Shared by the scalar and table-function helpers below;
// they only differ in which CreateXxxFunctionInfo wraps the result.
FunctionDescription BuildDescription(vector<LogicalType> parameter_types, const std::string &description,
                                     std::initializer_list<const char *> parameter_names,
                                     const std::vector<std::string> &examples,
                                     std::initializer_list<const char *> categories) {
	FunctionDescription fd;
	fd.description = description;
	for (const auto *name : parameter_names) {
		fd.parameter_names.emplace_back(name);
	}
	fd.parameter_types = std::move(parameter_types);
	for (const auto &ex : examples) {
		fd.examples.emplace_back(ex);
	}
	for (const auto *c : categories) {
		fd.categories.emplace_back(c);
	}
	return fd;
}

template <class InfoT>
void RegisterWithInfo(ExtensionLoader &loader, InfoT &&info, FunctionDescription &&fd, const std::string &alias_of) {
	if (!alias_of.empty()) {
		info.alias_of = alias_of;
	}
	info.descriptions.push_back(std::move(fd));
	loader.RegisterFunction(std::move(info));
}

} // namespace

const std::vector<DoctestEntry> &GetDoctestRegistry() {
	return MutableRegistry();
}

void RegisterDocumentedScalar(ExtensionLoader &loader, ScalarFunction function, const std::string &description,
                              std::initializer_list<const char *> parameter_names,
                              const std::vector<std::string> &examples, const std::string &alias_of,
                              std::initializer_list<const char *> categories,
                              const std::vector<std::string> &executable_examples) {
	const std::string function_name = function.name;
	auto fd = BuildDescription(function.arguments, description, parameter_names, examples, categories);
	RegisterWithInfo(loader, CreateScalarFunctionInfo(std::move(function)), std::move(fd), alias_of);
	AppendDoctests(function_name, executable_examples);
}

void RegisterDocumentedScalarSet(ExtensionLoader &loader, ScalarFunctionSet set, const std::string &description,
                                 std::initializer_list<DocumentedOverload> overloads,
                                 const std::vector<std::string> &examples, const std::string &alias_of,
                                 std::initializer_list<const char *> categories,
                                 const std::vector<std::string> &executable_examples) {
	const std::string function_name = set.name;
	CreateScalarFunctionInfo info(std::move(set));
	if (!alias_of.empty()) {
		info.alias_of = alias_of;
	}
	for (const auto &ov : overloads) {
		vector<LogicalType> types;
		for (auto id : ov.parameter_types) {
			types.emplace_back(id);
		}
		info.descriptions.push_back(BuildDescription(std::move(types), description, ov.parameter_names, examples,
		                                             categories));
	}
	loader.RegisterFunction(std::move(info));
	AppendDoctests(function_name, executable_examples);
}

void RegisterDocumentedScalarSet(ExtensionLoader &loader, ScalarFunctionSet set,
                                 std::initializer_list<FunctionDescription> per_overload_descriptions,
                                 const std::string &alias_of) {
	CreateScalarFunctionInfo info(std::move(set));
	if (!alias_of.empty()) {
		info.alias_of = alias_of;
	}
	for (const auto &d : per_overload_descriptions) {
		info.descriptions.push_back(d);
	}
	loader.RegisterFunction(std::move(info));
}

void RegisterDocumentedTableFunction(ExtensionLoader &loader, TableFunction function, const std::string &description,
                                     std::initializer_list<const char *> positional_parameter_names,
                                     const std::vector<std::string> &examples, const std::string &alias_of,
                                     std::initializer_list<const char *> categories,
                                     const std::vector<std::string> &executable_examples) {
	const std::string function_name = function.name;
	auto fd = BuildDescription(function.arguments, description, positional_parameter_names, examples, categories);
	RegisterWithInfo(loader, CreateTableFunctionInfo(std::move(function)), std::move(fd), alias_of);
	AppendDoctests(function_name, executable_examples);
}

void RegisterDocumentedAggregate(ExtensionLoader &loader, AggregateFunction function, const std::string &description,
                                 std::initializer_list<const char *> parameter_names,
                                 const std::vector<std::string> &examples, const std::string &alias_of,
                                 std::initializer_list<const char *> categories,
                                 const std::vector<std::string> &executable_examples) {
	const std::string function_name = function.name;
	auto fd = BuildDescription(function.arguments, description, parameter_names, examples, categories);
	RegisterWithInfo(loader, CreateAggregateFunctionInfo(std::move(function)), std::move(fd), alias_of);
	AppendDoctests(function_name, executable_examples);
}

namespace {

// SQL string-literal escape: wrap in single quotes, double any embedded
// single quotes. Intentionally minimal — input comes from C++ source code
// at compile time, not from user input.
std::string SqlQuote(const std::string &s) {
	std::string out;
	out.reserve(s.size() + 2);
	out += '\'';
	for (char c : s) {
		if (c == '\'') {
			out += "''";
		} else {
			out += c;
		}
	}
	out += '\'';
	return out;
}

} // namespace

namespace {

std::vector<CopyDocEntry> &MutableCopyDocsRegistry() {
	static std::vector<CopyDocEntry> registry;
	return registry;
}

} // namespace

const std::vector<CopyDocEntry> &GetCopyDocsRegistry() {
	return MutableCopyDocsRegistry();
}

void RegisterDocumentedCopyFunction(ExtensionLoader &loader, CopyFunction function, const std::string &description,
                                    const std::vector<std::string> &examples, const std::string &alias_of,
                                    std::initializer_list<const char *> categories) {
	CopyDocEntry entry;
	entry.name = function.name;
	entry.description = description;
	entry.examples = examples;
	for (const auto *c : categories) {
		entry.categories.emplace_back(c);
	}
	entry.alias_of = alias_of;
	MutableCopyDocsRegistry().push_back(std::move(entry));

	loader.RegisterFunction(std::move(function));
}

void RegisterCopyDocsMacro(ExtensionLoader &loader) {
	const auto &registry = GetCopyDocsRegistry();
	std::string sql = "CREATE OR REPLACE MACRO miint_documented_copy_functions() AS TABLE ";
	if (registry.empty()) {
		sql += "SELECT NULL::VARCHAR AS name, NULL::VARCHAR AS description, "
		       "NULL::VARCHAR[] AS examples, NULL::VARCHAR[] AS categories, "
		       "NULL::VARCHAR AS alias_of WHERE FALSE;";
	} else {
		auto sql_list = [](const std::vector<std::string> &xs) {
			std::string out = "[";
			for (size_t i = 0; i < xs.size(); ++i) {
				if (i > 0) {
					out += ", ";
				}
				out += SqlQuote(xs[i]);
			}
			out += "]";
			return out;
		};
		sql += "SELECT * FROM (VALUES ";
		for (size_t i = 0; i < registry.size(); ++i) {
			if (i > 0) {
				sql += ", ";
			}
			const auto &e = registry[i];
			sql += "(" + SqlQuote(e.name) + ", " + SqlQuote(e.description) + ", " + sql_list(e.examples) + ", " +
			       sql_list(e.categories) + ", " + SqlQuote(e.alias_of) + ")";
		}
		sql += ") AS t(name, description, examples, categories, alias_of);";
	}
	Connection con(loader.GetDatabaseInstance());
	auto result = con.Query(sql);
	if (result->HasError()) {
		throw InternalException("Failed to register miint_documented_copy_functions macro: %s", result->GetError());
	}
}

namespace {

std::vector<MacroDocEntry> &MutableMacroDocsRegistry() {
	static std::vector<MacroDocEntry> registry;
	return registry;
}

} // namespace

const std::vector<MacroDocEntry> &GetMacroDocsRegistry() {
	return MutableMacroDocsRegistry();
}

void RegisterDocumentedMacro(ExtensionLoader &loader, const std::string &name, const std::string &sql_body,
                             const std::string &description, const std::vector<std::string> &examples,
                             const std::string &alias_of, std::initializer_list<const char *> categories) {
	Connection con(loader.GetDatabaseInstance());
	auto result = con.Query(sql_body);
	if (result->HasError()) {
		throw InternalException("Failed to register macro '%s': %s", name, result->GetError());
	}

	MacroDocEntry entry;
	entry.name = name;
	entry.description = description;
	entry.examples = examples;
	for (const auto *c : categories) {
		entry.categories.emplace_back(c);
	}
	entry.alias_of = alias_of;
	MutableMacroDocsRegistry().push_back(std::move(entry));
}

void RegisterMacroDocsMacro(ExtensionLoader &loader) {
	const auto &registry = GetMacroDocsRegistry();
	std::string sql = "CREATE OR REPLACE MACRO miint_documented_macros() AS TABLE ";
	if (registry.empty()) {
		sql += "SELECT NULL::VARCHAR AS name, NULL::VARCHAR AS description, "
		       "NULL::VARCHAR[] AS examples, NULL::VARCHAR[] AS categories, "
		       "NULL::VARCHAR AS alias_of WHERE FALSE;";
	} else {
		auto sql_list = [](const std::vector<std::string> &xs) {
			std::string out = "[";
			for (size_t i = 0; i < xs.size(); ++i) {
				if (i > 0) {
					out += ", ";
				}
				out += SqlQuote(xs[i]);
			}
			out += "]";
			return out;
		};
		sql += "SELECT * FROM (VALUES ";
		for (size_t i = 0; i < registry.size(); ++i) {
			if (i > 0) {
				sql += ", ";
			}
			const auto &e = registry[i];
			sql += "(" + SqlQuote(e.name) + ", " + SqlQuote(e.description) + ", " + sql_list(e.examples) + ", " +
			       sql_list(e.categories) + ", " + SqlQuote(e.alias_of) + ")";
		}
		sql += ") AS t(name, description, examples, categories, alias_of);";
	}
	Connection con(loader.GetDatabaseInstance());
	auto result = con.Query(sql);
	if (result->HasError()) {
		throw InternalException("Failed to register miint_documented_macros macro: %s", result->GetError());
	}
}

void RegisterDoctestMacro(ExtensionLoader &loader) {
	const auto &registry = GetDoctestRegistry();
	std::string sql = "CREATE OR REPLACE MACRO miint_doctest_examples() AS TABLE ";
	if (registry.empty()) {
		sql += "SELECT NULL::VARCHAR AS function_name, NULL::VARCHAR AS sql WHERE FALSE;";
	} else {
		sql += "SELECT * FROM (VALUES ";
		for (size_t i = 0; i < registry.size(); ++i) {
			if (i > 0) {
				sql += ", ";
			}
			sql += "(" + SqlQuote(registry[i].function_name) + ", " + SqlQuote(registry[i].sql) + ")";
		}
		sql += ") AS t(function_name, sql);";
	}
	Connection con(loader.GetDatabaseInstance());
	auto result = con.Query(sql);
	if (result->HasError()) {
		throw InternalException("Failed to register miint_doctest_examples macro: %s", result->GetError());
	}
}

} // namespace duckdb
