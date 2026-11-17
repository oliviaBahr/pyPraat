#pragma once

#include <mutex>
#include <string>

#include <pybind11/pybind11.h>

void ensure_praat();
std::string praat_error_text();
std::mutex& praat_mutex();
void bind_object(pybind11::module_& module);
void bind_invoke(pybind11::module_& module);
