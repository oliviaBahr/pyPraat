#include <pybind11/pybind11.h>

#include <stdexcept>
#include <string>

#include "object.h"

namespace py = pybind11;

PraatObject::PraatObject(autoDaata given) : data(std::move(given)) {}

std::string PraatObject::class_name() const {
	if (!data)
		throw std::runtime_error("This Praat object no longer exists.");
	conststring32 text = Thing_className(data.get());
	return text ? std::string(Melder_peek32to8(text)) : std::string();
}

std::string PraatObject::name() const {
	if (!data)
		throw std::runtime_error("This Praat object no longer exists.");
	conststring32 text = data->name.get();
	return text ? std::string(Melder_peek32to8(text)) : std::string();
}

void bind_object(py::module_& module) {
	py::class_<PraatObject, std::unique_ptr<PraatObject>>(module, "Object")
	    .def_property_readonly("class_name", &PraatObject::class_name,
	                           "Praat class name, such as Sound or Pitch.")
	    .def_property_readonly("name", &PraatObject::name, "Name of this object, without the class prefix.");
}
