import pytest

from pypraat import Object, invoke


def tone():
    return invoke(
        [],
        "Create Sound from formula...",
        "tone",
        1,
        0.0,
        0.5,
        44100,
        "0.5 * sin(2*pi*200*x)",
    )


def test_create_sound_class():
    sound = tone()
    assert isinstance(sound, Object)
    assert sound.class_name == "Sound"


def test_to_pitch():
    pitch = invoke([tone()], "To Pitch...", 0.0, 75, 600)
    assert isinstance(pitch, Object)
    assert pitch.class_name == "Pitch"


def test_get_value_at_time():
    pitch = invoke([tone()], "To Pitch...", 0.0, 75, 600)
    value = invoke([pitch], "Get value at time...", 0.25, "Hertz", "linear")
    assert type(value) is float
    assert 150 < value < 250


def test_bad_argument_raises_praat_text():
    with pytest.raises(RuntimeError, match="greater than 0"):
        invoke([tone()], "To Pitch...", 0.0, -1, 600)


def test_invokes_do_not_share_objects():
    first = invoke([], "Create Sound from formula...", "a", 1, 0, 1, 44100, "0")
    second = invoke([], "Create Sound from formula...", "b", 1, 0, 1, 16000, "0")
    assert invoke([first], "Get sampling frequency") == 44100
    assert invoke([second], "Get sampling frequency") == 16000
    with pytest.raises(RuntimeError):
        invoke([], "Get sampling frequency")


def test_quoted_string():
    strings = invoke([], "Create Strings from texts...", "quote", ['say "hi"'])
    assert strings.class_name == "Strings"
    assert invoke([strings], "Get string...", 1) == 'say "hi"'


def test_omitted_argument_is_not_invented():
    # Praat's colon form has no "use the form default" token. Leaving the
    # ceiling out must stay left out, so Praat itself reports it missing.
    with pytest.raises(RuntimeError, match="Pitch ceiling"):
        invoke([tone()], "To Pitch...", 0.0, 75)


def test_integer_result_and_random_state_is_parked():
    first = invoke(
        [],
        "Create Sound from formula...",
        "a",
        1,
        0,
        1,
        44100,
        "randomGauss(0,1)",
    )
    count = invoke([first], "Get number of samples")
    assert type(count) is int
    assert count == 44100
    mean_first = invoke([first], "Get mean...", 0, 0, 0)
    second = invoke(
        [],
        "Create Sound from formula...",
        "b",
        1,
        0,
        1,
        44100,
        "randomGauss(0,1)",
    )
    mean_second = invoke([second], "Get mean...", 0, 0, 0)
    assert type(mean_first) is float
    assert mean_first == mean_second
