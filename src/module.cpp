#include "internal.h"

#include <stdexcept>
#include <string>

#include "Data.h"
#include "Interpreter.h"
#include "ManPages.h"
#include "main_Praat.h"
#include "melder.h"
#include "praat.h"
#include "praatP.h"
#include "praat_script.h"
#include "praat_uvafon_init.h"

namespace py = pybind11;

std::string praat_error_text() {
	conststring32 message = Melder_getError();
	std::string text = message ? Melder_peek32to8(message) : "Praat error";
	Melder_clearError();
	return text;
}

void ensure_praat() {
	static bool ready = false;
	if (ready)
		return;
	MelderConsole_init();
	praatlib_init();
	Melder_setAppName(U"Praat");
	Melder_setAppVersion(U"" stringize(PRAAT_VERSION_STR), PRAAT_VERSION_NUM);
	Melder_setAppDate(PRAAT_YEAR, PRAAT_MONTH, PRAAT_DAY);
	Melder_setAppContactAddress(U"paul.boersma", U"uva.nl");
	theCurrentPraatApplication->manPages = ManPages_create().releaseToAmbiguousOwner();
	Data_setPublishProc([](autoDaata me) -> int {
		try {
			praat_new(me.move(), U"");
			praat_updateSelection();
			return 1;
		} catch (MelderError) {
			Melder_throw(U"Not published.");
		}
	});
	praat_uvafon_init();
	praat_picture_init(false);
	// Menu commands invoked from Python have an interpreter but no script.
	// Praat 7 treats that as untrusted and refuses to save files. A script
	// that arrived through run_file still goes through the normal check.
	Melder_setTrustProc([](void* void_interpreter, conststring32 message) {
		Interpreter interpreter = static_cast<Interpreter>(void_interpreter);
		if (interpreter && !interpreter->scriptReference && !interpreter->notebookReference)
			return;
		MelderTrust::_defaultProc(void_interpreter, message);
	});
	ready = true;
}

void run_file(const std::string& path) {
	std::lock_guard<std::recursive_mutex> lock(praat_mutex());
	ensure_praat();
	Melder_rememberShellDirectory();
	try {
		// praat_runScript is the nested `runScript` call and needs a parent
		// interpreter. `praat --run` enters a file through this function.
		// The batch suite saves audio, which Praat 7 allows only with full trust.
		autostring32 fileName = Melder_8to32_e(path.c_str());
		praat_executeScriptFromCommandLine(fileName.get(), 0, nullptr, true);
	} catch (MelderError) {
		throw std::runtime_error(praat_error_text());
	}
}

PYBIND11_MODULE(_pypraat, module) {
	module.doc() = "Praat, compiled in-process.";
	bind_object(module);
	bind_invoke(module);
	bind_catalog(module);
	bind_trace(module);
	module.def("run_file", &run_file, py::arg("path"),
	           "Run a Praat script file through the script interpreter.");
}
