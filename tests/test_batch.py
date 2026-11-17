import os
from pathlib import Path

import pypraat

PRAAT_TEST = Path(__file__).resolve().parents[1] / "praat" / "test"


def test_run_all_tests_batch(capfd):
    os.chdir(PRAAT_TEST)
    pypraat.run_file(str(PRAAT_TEST / "runAllTests_batch.praat"))
    captured = capfd.readouterr()
    assert "ALL PRAAT TESTS WENT OK" in captured.out
