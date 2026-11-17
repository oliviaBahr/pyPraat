#include "internal.h"

#include <pybind11/numpy.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "object.h"

#include "Interpreter.h"
#include "NUMrandom.h"
#include "melder.h"
#include "praat.h"
#include "praatP.h"
#include "praat_script.h"

namespace py = pybind11;

std::mutex& praat_mutex() {
	static std::mutex mutex;
	return mutex;
}

namespace {

struct MelderStringOwner {
	MelderString text{};
	~MelderStringOwner() { MelderString_free(&text); }
	MelderStringOwner() = default;
	MelderStringOwner(const MelderStringOwner&) = delete;
	MelderStringOwner& operator=(const MelderStringOwner&) = delete;
};

std::string utf8_of(conststring32 text) {
	if (!text || !text[0])
		return {};
	return std::string(Melder_peek32to8(text));
}

void append_quoted(MelderString* line, const std::string& utf8) {
	autostring32 text = Melder_8to32_e(utf8.c_str());
	MelderString_appendCharacter(line, U'"');
	for (const char32* character = text.get(); character && *character; character++) {
		if (*character == U'"')
			MelderString_append(line, U"\"\"");
		else
			MelderString_appendCharacter(line, *character);
	}
	MelderString_appendCharacter(line, U'"');
}

void append_argument(MelderString* line, py::handle argument);

void append_sequence(MelderString* line, py::handle sequence) {
	MelderString_appendCharacter(line, U'{');
	bool first = true;
	for (py::handle item : sequence) {
		if (!first)
			MelderString_append(line, U", ");
		else
			MelderString_appendCharacter(line, U' ');
		first = false;
		append_argument(line, item);
	}
	MelderString_append(line, first ? U"}" : U" }");
}

void append_argument(MelderString* line, py::handle argument) {
	if (argument.is_none())
		throw std::invalid_argument("only trailing arguments can be omitted");
	if (py::isinstance<PraatObject>(argument))
		throw std::invalid_argument("Praat objects are selected, not command arguments");
	if (py::isinstance<py::bool_>(argument)) {
		MelderString_append(line, argument.cast<bool>() ? U"1" : U"0");
	} else if (py::isinstance<py::int_>(argument)) {
		int overflow = 0;
		long long value = PyLong_AsLongLongAndOverflow(argument.ptr(), &overflow);
		if (overflow || (value == -1 && PyErr_Occurred())) {
			PyErr_Clear();
			throw std::invalid_argument("integer argument does not fit in 64 bits");
		}
		MelderString_append(line, Melder_integer(static_cast<int64>(value)));
	} else if (py::isinstance<py::float_>(argument)) {
		double value = argument.cast<double>();
		if (!std::isfinite(value))
			MelderString_append(line, U"--undefined--");
		else
			MelderString_append(line, Melder_double(value));
	} else if (py::isinstance<py::str>(argument)) {
		append_quoted(line, argument.cast<std::string>());
	} else if (py::isinstance<py::list>(argument) || py::isinstance<py::tuple>(argument)) {
		append_sequence(line, argument);
	} else {
		throw std::invalid_argument("unsupported Praat command argument");
	}
}

autostring32 format_command(const std::string& title, const py::args& arguments) {
	std::vector<py::handle> items;
	items.reserve(arguments.size());
	for (py::handle argument : arguments)
		items.push_back(argument);
	while (!items.empty() && items.back().is_none())
		items.pop_back();

	autostring32 title32 = Melder_8to32_e(title.c_str());
	MelderStringOwner line;
	if (items.empty()) {
		MelderString_append(&line.text, title32.get());
	} else {
		char32* chars = title32.get();
		integer length = Melder_length(chars);
		if (length >= 3 && chars[length - 3] == U'.' && chars[length - 2] == U'.' &&
		    chars[length - 1] == U'.')
			chars[length - 3] = U'\0';
		MelderString_append(&line.text, chars, U": ");
		bool first = true;
		for (py::handle item : items) {
			if (!first)
				MelderString_append(&line.text, U", ");
			first = false;
			append_argument(&line.text, item);
		}
	}
	return Melder_dup(line.text.string ? line.text.string : U"");
}

struct HeldSlot {
	ClassInfo klas = nullptr;
	Daata object = nullptr;
	autostring32 name;
	structMelderFile file{};
	integer id = 0;
	bool isSelected = false;
	Editor editors[praat_MAXNUM_EDITORS]{};
	bool isBeingCreated = false;
};

HeldSlot take_slot(structPraat_Object& slot) {
	HeldSlot held;
	held.klas = slot.klas;
	held.object = slot.object;
	held.name = std::move(slot.name);
	held.file = slot.file;
	held.id = slot.id;
	held.isSelected = slot.isSelected;
	held.isBeingCreated = slot.isBeingCreated;
	for (int editor = 0; editor < praat_MAXNUM_EDITORS; editor++) {
		held.editors[editor] = slot.editors[editor];
		slot.editors[editor] = nullptr;
	}
	slot.klas = nullptr;
	slot.object = nullptr;
	slot.id = 0;
	slot.isSelected = false;
	slot.isBeingCreated = false;
	MelderFile_setToNull(&slot.file);
	return held;
}

void put_slot(structPraat_Object& slot, HeldSlot& held) {
	slot.klas = held.klas;
	slot.object = held.object;
	held.object = nullptr;
	slot.name = std::move(held.name);
	slot.file = held.file;
	slot.id = held.id;
	slot.isSelected = held.isSelected;
	slot.isBeingCreated = held.isBeingCreated;
	for (int editor = 0; editor < praat_MAXNUM_EDITORS; editor++)
		slot.editors[editor] = held.editors[editor];
}

struct Borrowed {
	PraatObject* owner = nullptr;
	Daata raw = nullptr;
	bool inserted = false;
};

struct Session {
	std::vector<HeldSlot> parked;
	std::vector<Borrowed> borrowed;
	std::vector<autoDaata> created;
	integer saved_count = 0;
	integer saved_selection = 0;
	integer saved_being_created = 0;
	integer saved_latest_id = 0;
	integer saved_selected[1 + structPraatObjects::MAXNUM_DATA_CLASSES]{};
	bool objects_parked = false;

	MelderStringOwner capture;
	MelderInfo::Proc saved_proc = nullptr;
	MelderString* saved_buffer = nullptr;
	autostring32 saved_foreground;
	bool info_parked = false;

	std::vector<unsigned char> random_bytes;
	integer saved_channel = 0;
	bool random_parked = false;
	bool finished = false;

	Session() {
		park_random();
		park_info();
		park_objects();
	}

	~Session() {
		try {
			if (!finished && objects_parked)
				reclaim();
			restore();
		} catch (MelderError) {
			Melder_clearError();
		} catch (...) {
		}
	}

	Session(const Session&) = delete;
	Session& operator=(const Session&) = delete;

	void park_random() {
		random_bytes.resize(static_cast<size_t>(NUMrandom_statesSize()));
		NUMrandom_saveStates(random_bytes.data());
		saved_channel = NUMrandom_getChannel();
		random_parked = true;
	}

	void park_info() {
		saved_proc = MelderInfo::_p_currentProc;
		saved_buffer = MelderInfo::_p_currentBuffer;
		conststring32 foreground = MelderInfo::_foregroundBuffer.string;
		saved_foreground = Melder_dup(foreground ? foreground : U"");
		MelderString_empty(&capture.text);
		Melder_divertInfo(&capture.text);
		info_parked = true;
	}

	void park_objects() {
		structPraatObjects* list = theCurrentPraatObjects;
		integer count = list->n;
		parked.reserve(static_cast<size_t>(count));
		for (integer index = 1; index <= count; index++)
			parked.push_back(take_slot(list->list[index]));
		saved_count = count;
		saved_selection = list->totalSelection;
		saved_being_created = list->totalBeingCreated;
		saved_latest_id = list->sequentialUniqueIdOfLatestObjectInList;
		for (integer klas = 1; klas <= structPraatObjects::MAXNUM_DATA_CLASSES; klas++)
			saved_selected[klas] = list->numberOfSelected[klas];
		list->n = 0;
		list->totalSelection = 0;
		list->totalBeingCreated = 0;
		for (integer klas = 1; klas <= structPraatObjects::MAXNUM_DATA_CLASSES; klas++)
			list->numberOfSelected[klas] = 0;
		objects_parked = true;
	}

	void select_one(py::handle item) {
		PraatObject* owner = item.cast<PraatObject*>();
		if (!owner->data)
			throw std::runtime_error("This Praat object no longer exists.");
		Daata raw = owner->data.releaseToAmbiguousOwner();
		borrowed.push_back(Borrowed{owner, raw, false});
		insert_borrowed(raw);
		borrowed.back().inserted = true;
		praat_select(theCurrentPraatObjects->n);
	}

	void select_receivers(py::handle receivers) {
		if (!receivers.is_none()) {
			if (py::isinstance<PraatObject>(receivers))
				select_one(receivers);
			else
				for (py::handle item : receivers)
					select_one(item);
		}
		// Menu dispatch runs a command only when its executable flag matches the
		// selection. praat_select does not refresh those flags; praat_show does.
		praat_show();
	}

	void insert_borrowed(Daata raw) {
		structPraatObjects* list = theCurrentPraatObjects;
		if (list->n == praat_MAXNUM_OBJECTS)
			throw std::runtime_error("The Praat object list is full.");
		integer index = ++list->n;
		structPraat_Object& slot = list->list[index];
		slot.klas = nullptr;
		slot.object = nullptr;
		slot.name.reset();
		MelderFile_setToNull(&slot.file);
		slot.id = 0;
		slot.isSelected = false;
		slot.isBeingCreated = false;
		for (int editor = 0; editor < praat_MAXNUM_EDITORS; editor++)
			slot.editors[editor] = nullptr;

		autoMelderString given, full;
		conststring32 current = raw->name.get();
		MelderString_copy(&given, current && current[0] ? current : U"untitled");
		praat_cleanUpName(given.string);
		MelderString_append(&full, Thing_className(raw), U" ", given.string);
		slot.name = Melder_dup_f(full.string);
		slot.klas = raw->classInfo;
		slot.object = raw;
		slot.id = ++list->sequentialUniqueIdOfLatestObjectInList;
	}

	void reclaim() {
		if (finished)
			return;
		structPraatObjects* list = theCurrentPraatObjects;
		for (integer index = 1; index <= list->n; index++) {
			structPraat_Object& slot = list->list[index];
			Daata raw = slot.object;
			slot.object = nullptr;
			slot.name.reset();
			slot.klas = nullptr;
			slot.isSelected = false;
			slot.isBeingCreated = false;
			for (int editor = 0; editor < praat_MAXNUM_EDITORS; editor++)
				slot.editors[editor] = nullptr;
			MelderFile_setToNull(&slot.file);
			if (!raw)
				continue;
			bool was_borrowed = false;
			for (Borrowed& item : borrowed) {
				if (item.raw == raw && item.owner) {
					item.owner->data.adoptFromAmbiguousOwner(raw);
					item.owner = nullptr;
					was_borrowed = true;
					break;
				}
			}
			if (!was_borrowed) {
				autoDaata taken;
				taken.adoptFromAmbiguousOwner(raw);
				created.push_back(std::move(taken));
			}
		}
		list->n = 0;
		list->totalSelection = 0;
		list->totalBeingCreated = 0;
		for (integer klas = 1; klas <= structPraatObjects::MAXNUM_DATA_CLASSES; klas++)
			list->numberOfSelected[klas] = 0;
		for (Borrowed& item : borrowed) {
			if (item.owner && !item.inserted) {
				item.owner->data.adoptFromAmbiguousOwner(item.raw);
				item.owner = nullptr;
			}
		}
		finished = true;
	}

	void restore() {
		if (objects_parked) {
			structPraatObjects* list = theCurrentPraatObjects;
			for (size_t index = 0; index < parked.size(); index++)
				put_slot(list->list[index + 1], parked[index]);
			list->n = saved_count;
			list->totalSelection = saved_selection;
			list->totalBeingCreated = saved_being_created;
			list->sequentialUniqueIdOfLatestObjectInList = saved_latest_id;
			for (integer klas = 1; klas <= structPraatObjects::MAXNUM_DATA_CLASSES; klas++)
				list->numberOfSelected[klas] = saved_selected[klas];
			objects_parked = false;
			praat_show();
		}
		if (info_parked) {
			MelderString_copy(&MelderInfo::_foregroundBuffer,
			                  saved_foreground.get() ? saved_foreground.get() : U"");
			MelderInfo::_p_currentProc = saved_proc;
			MelderInfo::_p_currentBuffer = saved_buffer;
			info_parked = false;
		}
		if (random_parked) {
			NUMrandom_restoreStates(random_bytes.data());
			NUMrandom_setChannel(saved_channel);
			random_parked = false;
		}
	}

	py::object take_result(Interpreter interpreter) {
		autostring32 info = Melder_dup(Melder_getInfo());
		const auto kind = interpreter->returnType;
		autoVEC real_vector = std::move(interpreter->returnedRealVector);
		autoINTVEC integer_vector = std::move(interpreter->returnedIntegerVector);
		autoMAT real_matrix = std::move(interpreter->returnedRealMatrix);
		autoSTRVEC string_array = std::move(interpreter->returnedStringArray);
		autostring32 returned_string = std::move(interpreter->returnedString);
		reclaim();

		conststring32 text = info.get();
		if ((!text || !text[0]) && returned_string)
			text = returned_string.get();

		switch (kind) {
		case kInterpreter_ReturnType::VOID_:
			created.clear();
			return py::none();
		case kInterpreter_ReturnType::OBJECT_:
			return wrap_created();
		case kInterpreter_ReturnType::REAL_:
			created.clear();
			return py::float_(number_from(text, true));
		case kInterpreter_ReturnType::INTEGER_:
			created.clear();
			return py::int_(integer_from(text));
		case kInterpreter_ReturnType::BOOLEAN_:
			created.clear();
			return py::bool_(boolean_from(text));
		case kInterpreter_ReturnType::STRING_:
			created.clear();
			return py::str(utf8_of(text));
		case kInterpreter_ReturnType::REALVECTOR_:
			created.clear();
			return real_vector_from(real_vector);
		case kInterpreter_ReturnType::INTEGERVECTOR_:
			created.clear();
			return integer_vector_from(integer_vector);
		case kInterpreter_ReturnType::REALMATRIX_:
			created.clear();
			return real_matrix_from(real_matrix);
		case kInterpreter_ReturnType::STRINGARRAY_:
			created.clear();
			return string_array_from(string_array);
		default:
			throw std::runtime_error("Praat returned an unknown result kind.");
		}
	}

	py::object wrap_created() {
		if (created.empty())
			return py::none();
		if (created.size() == 1)
			return py::cast(std::make_unique<PraatObject>(std::move(created[0])));
		py::tuple out(created.size());
		for (size_t index = 0; index < created.size(); index++)
			out[index] = py::cast(std::make_unique<PraatObject>(std::move(created[index])));
		return out;
	}

	static double number_from(conststring32 text, bool undefined_is_nan) {
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

	static long long integer_from(conststring32 text) {
		double value = number_from(text, false);
		double rounded = std::round(value);
		double gap = std::fabs(value - rounded);
		if (gap > 1e-6 && gap > 1e-9 * std::fabs(value))
			throw std::runtime_error("Praat returned a non-integral integer.");
		return static_cast<long long>(rounded);
	}

	static bool boolean_from(conststring32 text) {
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

	static py::array_t<double> real_vector_from(const autoVEC& vec) {
		py::array_t<double> array(static_cast<py::ssize_t>(vec.size));
		auto view = array.mutable_unchecked<1>();
		for (integer index = 1; index <= vec.size; index++)
			view(static_cast<py::ssize_t>(index - 1)) = vec[index];
		return array;
	}

	static py::array_t<long long> integer_vector_from(const autoINTVEC& vec) {
		py::array_t<long long> array(static_cast<py::ssize_t>(vec.size));
		auto view = array.mutable_unchecked<1>();
		for (integer index = 1; index <= vec.size; index++)
			view(static_cast<py::ssize_t>(index - 1)) = static_cast<long long>(vec[index]);
		return array;
	}

	static py::array_t<double> real_matrix_from(const autoMAT& mat) {
		py::array_t<double> array({static_cast<py::ssize_t>(mat.nrow), static_cast<py::ssize_t>(mat.ncol)});
		auto view = array.mutable_unchecked<2>();
		auto raw = mat.get();
		for (integer row = 1; row <= mat.nrow; row++)
			for (integer column = 1; column <= mat.ncol; column++)
				view(static_cast<py::ssize_t>(row - 1), static_cast<py::ssize_t>(column - 1)) =
				    raw[row][column];
		return array;
	}

	static py::list string_array_from(autoSTRVEC& values) {
		py::list out;
		for (integer index = 1; index <= values.size; index++) {
			conststring32 item = values[index].get();
			out.append(item ? std::string(Melder_peek32to8(item)) : std::string());
		}
		return out;
	}
};

autoInterpreterStack& interpreter_stack() {
	static autoInterpreterStack stack = InterpreterStack_create(Editor(nullptr));
	return stack;
}

py::object invoke(py::handle receivers, const std::string& title, py::args arguments) {
	std::lock_guard<std::mutex> lock(praat_mutex());
	ensure_praat();
	try {
		Session session;
		session.select_receivers(receivers);
		autostring32 command = format_command(title, arguments);
		autoInterpreter interpreter =
		    Interpreter_createFromEnvironment(interpreter_stack().get(), Editor(nullptr), nullptr);
		bool succeeded = praat_executeCommand(interpreter.get(), command.get());
		if (Melder_hasError())
			throw MelderError();
		if (!succeeded)
			Melder_throw(U"Command failed.");
		return session.take_result(interpreter.get());
	} catch (MelderError) {
		throw std::runtime_error(praat_error_text());
	}
}

} // namespace

void bind_invoke(py::module_& module) {
	module.def("invoke", &invoke, py::arg("objects"), py::arg("title"),
	           "Run one Praat menu command. objects is the selection. "
	           "Trailing omitted arguments keep Praat's defaults.");
}
