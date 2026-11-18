"""Menu commands from the Praat suite replay through the generated methods."""

import os
from pathlib import Path

import pytest

import pypraat

ROOT = Path(__file__).resolve().parents[1] / "praat" / "test"
PRELUDE = """
Text writing preferences: "UTF-8"
Text reading preferences: "try UTF-8, then MacRoman"
random_initializeWithSeedUnsafelyButPredictably (5489)
Debug multi-threading: "yes", 8, 0, "no"
"""


def _scripts(folders):
    found = []
    for name in folders:
        folder = ROOT / name
        found.extend(folder.glob("*.praat"))
        for subfolder in folder.iterdir():
            if subfolder.is_dir():
                found.extend(subfolder.glob("*.praat"))
    return sorted(path for path in found if "_GUI_" not in path.name)


def _replay(folders, tmp_path):
    previous = os.getcwd()
    os.chdir(ROOT)
    prelude = tmp_path / "prelude.praat"
    prelude.write_text(PRELUDE, encoding="utf-8")
    try:
        for path in _scripts(folders):
            pypraat.run_file(str(prelude))
            try:
                pypraat.replay_file(str(path.relative_to(ROOT)))
            except RuntimeError as error:
                pytest.fail(f"{path.relative_to(ROOT)}: {error}")
    finally:
        os.chdir(previous)


def test_replay_script_and_fon(tmp_path):
    _replay(("script", "fon"), tmp_path)


@pytest.mark.slow
def test_replay_the_whole_suite(tmp_path):
    folders = [
        path.name
        for path in ROOT.iterdir()
        if path.is_dir() and path.name not in {"manually", "speed"}
    ]
    _replay(sorted(folders), tmp_path)
