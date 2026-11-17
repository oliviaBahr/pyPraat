"""Generated menu methods stay in step with the catalog."""

import importlib.util
from pathlib import Path

import pypraat
from pypraat import Object, create_sound_from_formula, invoke

ROOT = Path(__file__).resolve().parents[1]


def _generate():
    spec = importlib.util.spec_from_file_location(
        "pypraat_generate", ROOT / "codegen" / "generate.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_generated_commands_match_the_catalog():
    _generate().check()


def test_sound_to_pitch_matches_invoke(tmp_path):
    formula = "0.5 * sin(2*pi*200*x)"
    sound = create_sound_from_formula("tone", 1, 0, 0.5, 44100, formula)
    other = create_sound_from_formula("tone", 1, 0, 0.5, 44100, formula)
    assert isinstance(sound, pypraat.Sound)
    assert isinstance(sound, pypraat.Vector)
    pitch = sound.to_pitch(0.0, 75, 600)
    raw = invoke([other._raw], "To Pitch...", 0.0, 75, 600)
    assert isinstance(pitch, pypraat.Pitch)
    assert isinstance(raw, Object)
    assert raw.class_name == "Pitch"
    method_path = tmp_path / "method.txt"
    invoke_path = tmp_path / "invoke.txt"
    invoke([pitch._raw], "Save as text file...", str(method_path))
    invoke([raw], "Save as text file...", str(invoke_path))
    assert method_path.read_text() == invoke_path.read_text()
