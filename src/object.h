#pragma once

#include <string>

#include "Data.h"

// Owns one Praat object. The Python type is Object; this name stays clear of
// the OBJECT macro in praat.h.
class PraatObject {
  public:
	autoDaata data;

	explicit PraatObject(autoDaata given);
	std::string class_name() const;
	std::string name() const;
};
