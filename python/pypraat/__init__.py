"""In-process Praat."""

from pypraat._pypraat import Object, invoke, run_file
from pypraat._commands import *
from pypraat._commands import __all__ as _command_names

__all__ = ["Object", "invoke", "run_file", *_command_names]
