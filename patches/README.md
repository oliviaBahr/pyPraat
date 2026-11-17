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

Not yet. `theActions` is function-local, so a later commit adds
`praat_commandCatalog()`.

## Trace hook

Not yet. A later commit adds one function-pointer hook at the menu dispatch
inside `praat_executeCommand`.
