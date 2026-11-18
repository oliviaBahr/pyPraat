#include "internal.h"
#include "object.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <pybind11/numpy.h>

#include "NUMrandom.h"
#include "praatP.h"
#include "praat_script.h"

namespace py = pybind11;

namespace {

std::string utf8_of(conststring32 text) {
	if (!text || !text[0])
		return {};
	return std::string(Melder_peek32to8(text));
}

std::string text_of(Daata data) {
	if (!data || !Data_canWriteText(data))
		return {};
	static int counter = 0;
	auto path =
	    std::filesystem::temp_directory_path() / ("pypraat-trace-" + std::to_string(++counter) + ".txt");
	structMelderFile file{};
	autostring32 wide = Melder_8to32_e(path.string().c_str());
	Melder_pathToFile(wide.get(), &file);
	Data_writeToTextFile(data, &file);
	std::ifstream input(path);
	std::ostringstream buffer;
	buffer << input.rdbuf();
	std::filesystem::remove(path);
	return buffer.str();
}

double number_from(conststring32 text, bool undefined_is_nan) {
	if (!text || !text[0])
		throw std::runtime_error("Praat returned an empty number.");
	double value = Melder_atof(text);
	if (isundef(value)) {
		if (undefined_is_nan)
			return std::numeric_limits<double>::quiet_NaN();
		throw std::runtime_error("Praat returned an undefined number.");
	}
	return value;
}

long long integer_from(conststring32 text) {
	double value = number_from(text, false);
	double rounded = std::round(value);
	double gap = std::fabs(value - rounded);
	if (gap > 1e-6 && gap > 1e-9 * std::fabs(value))
		throw std::runtime_error("Praat returned a non-integral integer.");
	return static_cast<long long>(rounded);
}

bool boolean_from(conststring32 text) {
	if (!text)
		throw std::runtime_error("Praat returned an empty boolean.");
	while (*text == U' ' || *text == U'\t' || *text == U'\n')
		text++;
	if (str32equ(text, U"yes") || str32equ(text, U"on"))
		return true;
	if (str32equ(text, U"no") || str32equ(text, U"off"))
		return false;
	return number_from(text, false) != 0.0;
}

conststring32 result_text(conststring32 info, conststring32 returned) {
	if (info && info[0])
		return info;
	return returned;
}

py::array_t<double> copy_real_vector(const autoVEC& vec) {
	py::array_t<double> array(static_cast<py::ssize_t>(vec.size));
	auto view = array.mutable_unchecked<1>();
	for (integer index = 1; index <= vec.size; index++)
		view(static_cast<py::ssize_t>(index - 1)) = vec[index];
	return array;
}

py::array_t<long long> copy_integer_vector(const autoINTVEC& vec) {
	py::array_t<long long> array(static_cast<py::ssize_t>(vec.size));
	auto view = array.mutable_unchecked<1>();
	for (integer index = 1; index <= vec.size; index++)
		view(static_cast<py::ssize_t>(index - 1)) = static_cast<long long>(vec[index]);
	return array;
}

py::array_t<double> copy_real_matrix(const autoMAT& mat) {
	py::array_t<double> array({static_cast<py::ssize_t>(mat.nrow), static_cast<py::ssize_t>(mat.ncol)});
	auto view = array.mutable_unchecked<2>();
	auto raw = mat.get();
	for (integer row = 1; row <= mat.nrow; row++)
		for (integer column = 1; column <= mat.ncol; column++)
			view(static_cast<py::ssize_t>(row - 1), static_cast<py::ssize_t>(column - 1)) = raw[row][column];
	return array;
}

py::list copy_string_array(const autoSTRVEC& values) {
	py::list out;
	for (integer index = 1; index <= values.size; index++) {
		conststring32 item = values[index].get();
		out.append(item ? std::string(Melder_peek32to8(item)) : std::string());
	}
	return out;
}

py::object argument_value(Stackel argument) {
	switch (argument->which) {
	case Stackel_NUMBER: {
		double value = argument->number;
		if (std::isfinite(value) && value == std::round(value) && std::fabs(value) < 9.0e15)
			return py::int_(static_cast<long long>(value));
		return py::float_(value);
	}
	case Stackel_STRING:
		return py::str(utf8_of(argument->getString()));
	case Stackel_NUMERIC_VECTOR: {
		py::list out;
		for (integer index = 1; index <= argument->numericVector.size; index++)
			out.append(argument->numericVector[index]);
		return out;
	}
	case Stackel_NUMERIC_MATRIX: {
		py::list rows;
		auto raw = argument->numericMatrix;
		for (integer row = 1; row <= raw.nrow; row++) {
			py::list columns;
			for (integer column = 1; column <= raw.ncol; column++)
				columns.append(raw[row][column]);
			rows.append(columns);
		}
		return rows;
	}
	case Stackel_STRING_ARRAY: {
		py::list out;
		for (integer index = 1; index <= argument->stringArray.size; index++) {
			conststring32 item = argument->stringArray[index];
			out.append(item ? std::string(Melder_peek32to8(item)) : std::string());
		}
		return out;
	}
	default:
		throw std::runtime_error("Praat command argument has an unsupported kind.");
	}
}

const char* kind_name(kInterpreter_ReturnType kind) {
	switch (kind) {
	case kInterpreter_ReturnType::VOID_:
		return "void";
	case kInterpreter_ReturnType::OBJECT_:
		return "object";
	case kInterpreter_ReturnType::REAL_:
		return "real";
	case kInterpreter_ReturnType::INTEGER_:
		return "integer";
	case kInterpreter_ReturnType::BOOLEAN_:
		return "boolean";
	case kInterpreter_ReturnType::STRING_:
		return "string";
	case kInterpreter_ReturnType::REALVECTOR_:
		return "real_vector";
	case kInterpreter_ReturnType::INTEGERVECTOR_:
		return "integer_vector";
	case kInterpreter_ReturnType::REALMATRIX_:
		return "real_matrix";
	case kInterpreter_ReturnType::STRINGARRAY_:
		return "string_array";
	default:
		throw std::runtime_error("Praat returned an unknown result kind.");
	}
}

py::dict trace_record(conststring32 title, conststring32* selectedClassNames, integer numberOfSelected,
                      Daata* selectedCopies, bool colonForm, Stackel arguments, integer numberOfArguments,
                      conststring32 dotsArguments, kInterpreter_ReturnType returnType, conststring32 info,
                      Interpreter interpreter, Daata* created, integer numberOfCreated, Daata* context,
                      conststring32* contextNames, integer* contextIds, integer numberOfContext,
                      conststring32* selectedNames, integer* selectedIds) {
	py::list selected;
	for (integer index = 0; index < numberOfSelected; index++) {
		autoDaata owned;
		owned.adoptFromAmbiguousOwner(selectedCopies[index]);
		selectedCopies[index] = nullptr;
		selected.append(py::cast(std::make_unique<PraatObject>(std::move(owned))));
	}
	py::list classes;
	for (integer index = 0; index < numberOfSelected; index++)
		classes.append(utf8_of(selectedClassNames[index]));
	py::list args;
	if (colonForm) {
		for (integer index = 1; index <= numberOfArguments; index++)
			args.append(argument_value(&arguments[index]));
	}
	py::list created_texts;
	for (integer index = 0; index < numberOfCreated; index++)
		created_texts.append(text_of(created[index]));

	conststring32 returned = interpreter ? interpreter->returnedString.get() : nullptr;
	conststring32 text = result_text(info, returned);
	py::dict record;
	record["title"] = utf8_of(title);
	record["classes"] = classes;
	record["selected"] = selected;
	record["colon"] = colonForm;
	record["arguments"] = args;
	record["dots"] = colonForm || !dotsArguments ? py::object(py::none()) : py::str(utf8_of(dotsArguments));
	record["return_kind"] = kind_name(returnType);
	record["created_texts"] = created_texts;
	py::list context_objects;
	py::list context_names;
	py::list context_ids;
	for (integer index = 0; index < numberOfContext; index++) {
		context_objects.append(py::capsule(static_cast<void*>(context[index])));
		context_names.append(utf8_of(contextNames[index]));
		context_ids.append(contextIds ? contextIds[index] : 0);
	}
	py::list selected_names;
	py::list selected_ids;
	for (integer index = 0; index < numberOfSelected; index++) {
		selected_names.append(selectedNames ? utf8_of(selectedNames[index]) : std::string());
		selected_ids.append(selectedIds ? selectedIds[index] : 0);
	}
	record["context_objects"] = context_objects;
	record["context_names"] = context_names;
	record["context_ids"] = context_ids;
	record["selected_names"] = selected_names;
	record["selected_ids"] = selected_ids;
	switch (returnType) {
	case kInterpreter_ReturnType::REAL_:
		record["value"] = py::float_(number_from(text, true));
		break;
	case kInterpreter_ReturnType::INTEGER_:
		record["value"] = py::int_(integer_from(text));
		break;
	case kInterpreter_ReturnType::BOOLEAN_:
		record["value"] = py::bool_(boolean_from(text));
		break;
	case kInterpreter_ReturnType::STRING_:
		record["value"] = py::str(utf8_of(text));
		break;
	case kInterpreter_ReturnType::REALVECTOR_:
		record["value"] =
		    interpreter ? copy_real_vector(interpreter->returnedRealVector) : py::array_t<double>(0);
		break;
	case kInterpreter_ReturnType::INTEGERVECTOR_:
		record["value"] =
		    interpreter ? copy_integer_vector(interpreter->returnedIntegerVector) : py::array_t<long long>(0);
		break;
	case kInterpreter_ReturnType::REALMATRIX_:
		record["value"] =
		    interpreter ? copy_real_matrix(interpreter->returnedRealMatrix) : py::array_t<double>({0, 0});
		break;
	case kInterpreter_ReturnType::STRINGARRAY_:
		record["value"] = interpreter ? copy_string_array(interpreter->returnedStringArray) : py::list();
		break;
	default:
		record["value"] = py::none();
		break;
	}
	return record;
}

struct TraceClosure {
	py::function* callback = nullptr;
	std::string error;
};

void trace_hook(conststring32 title, conststring32* selectedClassNames, integer numberOfSelected,
                Daata* selectedCopies, bool colonForm, Stackel arguments, integer numberOfArguments,
                conststring32 dotsArguments, kInterpreter_ReturnType returnType, conststring32 info,
                Interpreter interpreter, Daata* created, integer numberOfCreated, Daata* context,
                conststring32* contextNames, integer* contextIds, integer numberOfContext,
                conststring32* selectedNames, integer* selectedIds, void* closure) {
	auto* state = static_cast<TraceClosure*>(closure);
	if (!state->error.empty()) {
		for (integer index = 0; index < numberOfSelected; index++)
			if (selectedCopies[index])
				forget(selectedCopies[index]);
		return;
	}
	borrow_script_interpreter(interpreter);
	try {
		py::dict record = trace_record(title, selectedClassNames, numberOfSelected, selectedCopies, colonForm,
		                               arguments, numberOfArguments, dotsArguments, returnType, info,
		                               interpreter, created, numberOfCreated, context, contextNames,
		                               contextIds, numberOfContext, selectedNames, selectedIds);
		(*state->callback)(record);
	} catch (py::error_already_set& error) {
		state->error = error.what();
	} catch (MelderError) {
		state->error = praat_error_text();
	} catch (const std::exception& error) {
		state->error = error.what();
	}
	borrow_script_interpreter(nullptr);
	PyErr_Clear();
	for (integer index = 0; index < numberOfSelected; index++)
		if (selectedCopies && selectedCopies[index])
			forget(selectedCopies[index]);
}

void replay_file(const std::string& path, py::function callback) {
	std::lock_guard<std::recursive_mutex> lock(praat_mutex());
	ensure_praat();
	Melder_rememberShellDirectory();
	TraceClosure state;
	state.callback = &callback;
	praat_setMenuTraceHook(&trace_hook, &state);
	try {
		autostring32 fileName = Melder_8to32_e(path.c_str());
		praat_executeScriptFromCommandLine(fileName.get(), 0, nullptr, true);
	} catch (MelderError) {
		praat_setMenuTraceHook(nullptr, nullptr);
		if (!state.error.empty())
			throw std::runtime_error(state.error);
		throw std::runtime_error(praat_error_text());
	} catch (...) {
		praat_setMenuTraceHook(nullptr, nullptr);
		throw;
	}
	praat_setMenuTraceHook(nullptr, nullptr);
	if (!state.error.empty())
		throw std::runtime_error(state.error);
}

std::string object_text(PraatObject& object) {
	std::lock_guard<std::recursive_mutex> lock(praat_mutex());
	ensure_praat();
	if (!object.data)
		throw std::runtime_error("This Praat object no longer exists.");
	try {
		return text_of(object.data.get());
	} catch (MelderError) {
		throw std::runtime_error(praat_error_text());
	}
}

} // namespace

void bind_trace(py::module_& module) {
	module.def("replay_file", &replay_file, py::arg("path"), py::arg("callback"),
	           "Run a script and call back after every menu command.");
	module.def("object_text", &object_text, py::arg("object"), "Praat text serialization of one object.");
}
