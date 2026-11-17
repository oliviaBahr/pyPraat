#include "internal.h"

#include <string>
#include <vector>

#include "Interpreter.h"
#include "praat.h"
#include "praatP.h"
#include "praat_catalog.h"

namespace py = pybind11;

namespace {

std::string utf8_of(conststring32 text) {
	if (!text || !text[0])
		return {};
	return std::string(Melder_peek32to8(text));
}

py::object text_or_none(conststring32 text) {
	if (!text)
		return py::none();
	return py::str(utf8_of(text));
}

void collect_command(const PraatCommandRecord* command, const PraatFieldRecord* fields,
                     integer numberOfFields, void* closure) {
	auto* commands = static_cast<py::list*>(closure);
	py::dict record;
	record["title"] = utf8_of(command->title);
	record["callback"] = text_or_none(command->callbackName);
	record["window"] = text_or_none(command->window);
	record["menu"] = text_or_none(command->menu);
	record["hidden"] = command->hidden;
	record["depth"] = command->depth;
	py::list classes;
	py::list counts;
	for (int index = 0; index < 4; index++) {
		if (!command->classes[index])
			continue;
		classes.append(utf8_of(command->classes[index]));
		counts.append(command->counts[index]);
	}
	record["classes"] = classes;
	record["counts"] = counts;
	py::list field_list;
	for (integer index = 0; index < numberOfFields; index++) {
		const PraatFieldRecord& field = fields[index];
		py::dict item;
		item["kind"] = utf8_of(field.kind);
		item["name"] = text_or_none(field.name);
		item["label"] = text_or_none(field.label);
		item["default"] = text_or_none(field.defaultText);
		py::list options;
		for (integer option = 0; option < field.numberOfOptions; option++)
			options.append(utf8_of(field.options[option]));
		item["options"] = options;
		field_list.append(item);
	}
	record["fields"] = field_list;
	commands->append(record);
}

void collect_class(conststring32 name, conststring32 parent, void* closure) {
	auto* classes = static_cast<py::list*>(closure);
	py::dict record;
	record["name"] = utf8_of(name);
	record["parent"] = text_or_none(parent);
	classes->append(record);
}

py::list command_catalog() {
	std::lock_guard<std::mutex> lock(praat_mutex());
	ensure_praat();
	py::list commands;
	praat_commandCatalog(&collect_command, &commands);
	return commands;
}

py::list class_catalog() {
	std::lock_guard<std::mutex> lock(praat_mutex());
	ensure_praat();
	py::list classes;
	Thing_classCatalog(&collect_class, &classes);
	return classes;
}

} // namespace

void bind_catalog(py::module_& module) {
	module.def("command_catalog", &command_catalog, "Praat menu commands and the fields of each form.");
	module.def("class_catalog", &class_catalog, "Praat Thing classes and their parents.");
}
