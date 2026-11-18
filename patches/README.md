# Praat patches

`praat/` is a git submodule pinned to an upstream commit. Hooks that Praat
does not already export live as commits on a branch of that submodule.

## Batch init

Upstream `praatlib_init()` in `sys/praat.cpp` already starts Praat in batch
mode and installs the fixed menus without opening a window. pypraat calls
that, then `praat_uvafon_init()` and `praat_picture_init(false)`. No patch.

## Random state

`melder/NUMrandom.cpp` keeps the MT19937-64 state in a file-scope array with
no public save. `NUMrandom_saveStates` / `NUMrandom_restoreStates` copy that
array so `invoke` can park it. The per-thread channel stays on
`NUMrandom_getChannel` / `NUMrandom_setChannel`.

## Command catalog

`praat_commandCatalog()` walks object actions and fixed menu commands. A
title containing `...` is a form: the callback is called with `narg == -1`,
`UiForm_info` records the form, and the catalog reads each `UiField` (kind,
name, label, default, options). A file dialog has no fields; it is reported
as one `fileName` argument. `DIRECT` commands return immediately for a
negative `narg`, so the catalog does not run them. `Thing_classCatalog()`
lists readable classes and their parents.

## Trace hook

`praat_setMenuTraceHook` installs one function pointer. `praat_executeCommand`
calls it once, after a menu command has run and before `praat_updateSelection`.
The call carries the title, the classes and copies of the objects that were
selected, the list names and ids of that selection, the other objects that
were in the list (so a formula can name them), the arguments,
`kInterpreter_ReturnType`, the info text, and the objects the command created.
The copies belong to the hook. Directives such as `select` and `print` are not
menu commands and are not reported.
