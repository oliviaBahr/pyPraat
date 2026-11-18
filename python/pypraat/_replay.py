"""Replay a script's menu commands through the generated methods."""

import inspect
import math

import numpy

import pypraat._commands as commands
import pypraat._pypraat as raw

_SKIP_FIELDS = {"heading", "comment", "caption"}
_CATALOG = None
_FUNCTIONS = None


def _catalog():
    global _CATALOG, _FUNCTIONS
    if _CATALOG is not None:
        return
    _CATALOG = [
        command
        for command in raw.command_catalog()
        if command["title"] and not _omitted(command)
    ]
    _FUNCTIONS = {}
    for name in commands.__all__:
        function = getattr(commands, name)
        title = getattr(function, "_praat_command", None)
        if title and not isinstance(function, type):
            _FUNCTIONS.setdefault(title, []).append(function)


def _omitted(command):
    title = command["title"]
    callback = command["callback"] or ""
    if "View & Edit" in title:
        return True
    if callback.startswith(("EDITOR_", "WINDOW_")):
        return True
    if title.startswith("-") or title.endswith(" -"):
        return True
    return False


def _is_a(class_name, want):
    cls = commands._BY_NAME.get(class_name)
    parent = commands._BY_NAME.get(want)
    return cls is not None and parent is not None and issubclass(cls, parent)


def _pattern_matches(command, class_names):
    remaining = list(class_names)
    for want, count in zip(command["classes"], command["counts"]):
        hits = [
            index
            for index, name in enumerate(remaining)
            if name is not None and _is_a(name, want)
        ]
        if count == 0:
            if not hits:
                return False
        elif len(hits) != count:
            return False
        for index in hits:
            remaining[index] = None
    return all(name is None for name in remaining)


def _which(title, class_names):
    _catalog()
    action = None
    menu = None
    for command in _CATALOG:
        if command["title"] != title:
            continue
        if command["classes"]:
            if action is None and _pattern_matches(command, class_names):
                action = command
        elif menu is None:
            menu = command
    return action or menu


def _method(class_name, title):
    cls = commands._BY_NAME.get(class_name)
    while cls is not None and cls is not object:
        for function in cls.__dict__.values():
            if getattr(function, "_praat_command", None) == title:
                return function
        cls = cls.__bases__[0]
    return None


def _object_slots(function):
    slots = []
    for parameter in inspect.signature(function).parameters.values():
        if parameter.name == "self":
            continue
        if parameter.default is not inspect.Parameter.empty:
            break
        slots.append(parameter.name)
    return slots


def _declared(stem):
    for name in commands._BY_NAME:
        if _snake(name) == stem:
            return name
    return None


def _assign(slots, selected):
    used = [False] * len(selected)
    arguments = []
    for slot in slots:
        sequence = slot.endswith("_objects")
        stem = slot[: -len("_objects")] if sequence else slot
        declared = _declared(stem)
        chosen = []
        for index, item in enumerate(selected):
            if used[index] or declared is None or not _is_a(item.class_name, declared):
                continue
            chosen.append(item)
            used[index] = True
            if not sequence:
                break
        if sequence:
            if not chosen:
                return None
            arguments.append(chosen)
        elif len(chosen) != 1:
            return None
        else:
            arguments.append(chosen[0])
    if not all(used):
        return None
    return arguments


def _snake(text):
    chars = []
    previous = ""
    for character in text:
        if character.isupper() and previous.isalnum() and not previous.isupper():
            chars.append("_")
        chars.append(character.lower() if character.isalnum() else "_")
        previous = character
    name = "".join(chars)
    while "__" in name:
        name = name.replace("__", "_")
    return name.strip("_")


def _function(title, selected):
    for function in _FUNCTIONS.get(title, []):
        assigned = _assign(_object_slots(function), selected)
        if assigned is not None:
            return function, assigned
    return None, None


def _fields(command):
    return [
        field
        for field in command["fields"]
        if field["kind"] not in _SKIP_FIELDS and field["name"]
    ]


def _split_dots(text, count):
    if count <= 0:
        return []
    tokens = []
    index = 0
    text = text or ""
    while len(tokens) < count - 1 and index < len(text):
        while index < len(text) and text[index] in " \t":
            index += 1
        if index >= len(text):
            break
        if text[index] == '"':
            index += 1
            pieces = []
            while index < len(text):
                if text[index] == '"':
                    if index + 1 < len(text) and text[index + 1] == '"':
                        pieces.append('"')
                        index += 2
                        continue
                    index += 1
                    break
                pieces.append(text[index])
                index += 1
            tokens.append("".join(pieces))
        else:
            start = index
            while index < len(text) and text[index] not in " \t":
                index += 1
            tokens.append(text[start:index])
    if len(tokens) == count - 1:
        tokens.append(text[index:].lstrip(" \t"))
    return tokens


def _number(text):
    if text in {"--undefined--", "undefined"}:
        return float("nan")
    try:
        return float(text)
    except ValueError:
        return raw.script_number(text)


_CHANNEL_WORDS = {"All", "Average", "Left", "Right", "Mono", "Stereo"}


def _coerce(kind, text):
    if kind in {"real", "positive", "nonnegative", "real_or_undefined"}:
        return _number(text)
    if kind in {"integer", "natural0", "natural1"}:
        return int(_number(text))
    if kind == "channel":
        # Dots scripts use the numeric variables mono and stereo, not the words.
        if text in _CHANNEL_WORDS:
            return text
        return int(_number(text))
    if kind == "boolean":
        return text in {"yes", "1", "on"}
    return text


def _arguments(record, command):
    if record["colon"]:
        return list(record["arguments"])
    tokens = _split_dots(record["dots"], len(_fields(command)))
    return [
        _coerce(field["kind"], token) for field, token in zip(_fields(command), tokens)
    ]


def _wrap(item):
    cls = commands._BY_NAME.get(item.class_name, commands.Thing)
    return cls(item)


def _boxed(result):
    if isinstance(result, raw.Object):
        return _wrap(result)
    if isinstance(result, tuple):
        return tuple(_boxed(item) for item in result)
    return result


def _one_text(item):
    return raw.object_text(getattr(item, "_raw", item))


def _texts(result):
    if result is None:
        return []
    if isinstance(result, tuple):
        return [_one_text(item) for item in result]
    return [_one_text(result)]


def _equal(left, right):
    if isinstance(left, float) and isinstance(right, float):
        if math.isnan(left) and math.isnan(right):
            return True
        return left == right
    if isinstance(left, numpy.ndarray) or isinstance(right, numpy.ndarray):
        return numpy.array_equal(left, right, equal_nan=True)
    if isinstance(left, str) and isinstance(right, str):
        return left.removesuffix("\n") == right.removesuffix("\n")
    if isinstance(left, list) and isinstance(right, list):
        return len(left) == len(right) and all(
            _equal(a, b) for a, b in zip(left, right)
        )
    return left == right


def _on_command(record):
    title = record["title"]
    class_names = list(record["classes"])
    command = _which(title, class_names)
    if command is None:
        return
    selected = [_wrap(item) for item in record["selected"]]
    arguments = _arguments(record, command)
    raw.set_replay_context(
        record["context_objects"],
        record["context_names"],
        record["context_ids"],
        record["selected_names"],
        record["selected_ids"],
    )
    try:
        _replay_command(record, command, selected, arguments, title, class_names)
    finally:
        raw.clear_replay_context()


def _replay_command(record, command, selected, arguments, title, class_names):
    if not command["classes"]:
        # Fixed buttons such as Copy have no class in the catalog and still
        # read the current selection.
        if selected:
            result = _boxed(
                raw.invoke([item._raw for item in selected], title, *arguments)
            )
        else:
            function, _assigned = _function(title, [])
            if function is None:
                raise AssertionError(f"{title}: no generated function")
            result = function(*arguments)
    elif len(selected) == 1 and (method := _method(selected[0].class_name, title)):
        result = method(selected[0], *arguments)
    else:
        function, assigned = _function(title, selected)
        if function is None:
            raise AssertionError(f"{title}: no generated function for {class_names}")
        result = function(*assigned, *arguments)
    if record["return_kind"] == "object":
        got = _texts(result)
        expected = list(record["created_texts"])
        if got != expected:

            def _first(left, right):
                left_lines, right_lines = left.splitlines(), right.splitlines()
                for index, (one, other) in enumerate(zip(left_lines, right_lines)):
                    if one != other:
                        return f"line {index + 1}: {one[:100]!r} vs {other[:100]!r}"
                return f"lengths {len(left_lines)} vs {len(right_lines)}"

            detail = _first(got[0], expected[0]) if got and expected else "missing"
            raise AssertionError(
                f"{title}: created objects differ\n"
                f"counts {len(got)} vs {len(expected)} sizes {[len(item) for item in got]} {[len(item) for item in expected]}\n"
                f"{detail}"
            )
        return
    if not _equal(result, record["value"]):
        raise AssertionError(
            f"{title}: {record['return_kind']} result differs ({result!r} != {record['value']!r})"
        )


def replay_file(path):
    """Run a script and replay each menu command through its generated method."""
    raw.replay_file(path, _on_command)
