"""Write the generated Praat command methods."""

import keyword
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMMANDS = ROOT / "python" / "pypraat" / "_commands.py"
STUB = ROOT / "python" / "pypraat" / "__init__.pyi"

_SKIP_FIELDS = {"heading", "comment", "caption"}


def snake(text):
    chars = []
    previous = ""
    for character in text.removesuffix("..."):
        if character.isupper() and previous.isalnum() and not previous.isupper():
            chars.append("_")
        if character.isalnum():
            chars.append(character.lower())
        else:
            chars.append("_")
        previous = character
    name = "".join(chars)
    while "__" in name:
        name = name.replace("__", "_")
    name = name.strip("_") or "command"
    if name[0].isdigit():
        name = "n_" + name
    if keyword.iskeyword(name):
        name += "_"
    return name


def unique(name, used):
    if name not in used:
        used.add(name)
        return name
    number = 2
    while f"{name}_{number}" in used:
        number += 1
    chosen = f"{name}_{number}"
    used.add(chosen)
    return chosen


def omitted(command):
    title = command["title"]
    callback = command["callback"] or ""
    if "View & Edit" in title:
        return True
    if callback.startswith(("EDITOR_", "WINDOW_")):
        return True
    if title.startswith("-") or title.endswith(" -"):
        return True
    return False


def field_names(fields):
    used = set()
    names = []
    for field in fields:
        if field["kind"] in _SKIP_FIELDS or not field["name"]:
            continue
        names.append(unique(snake(field["name"]), used))
    return names


def object_slots(command):
    slots = []
    used = set()
    for class_name, count in zip(command["classes"], command["counts"]):
        base = snake(class_name)
        if count == 1:
            slots.append((unique(base, used), False, class_name))
        else:
            slots.append((unique(base + "_objects", used), True, class_name))
    return slots


def class_order(rows):
    parents = {row["name"]: row["parent"] for row in rows}
    seen = set()
    ordered = []

    def visit(name):
        if name in seen or name not in parents:
            return
        parent = parents[name]
        if parent:
            visit(parent)
        seen.add(name)
        ordered.append(name)

    for name in sorted(parents):
        visit(name)
    return ordered, parents


def _signature(object_params, fields, self_name=None):
    parts = []
    if self_name:
        parts.append("self")
    parts.extend(object_params)
    parts.extend(f"{name}=None" for name in fields)
    return ", ".join(parts)


def _call(title, receivers, fields):
    given = ", ".join(fields)
    return f"_wrap(invoke({receivers}, {title!r}, *_given({given})))"


def render():
    import pypraat._pypraat as raw

    classes, parents = class_order(raw.class_catalog())
    commands = [command for command in raw.command_catalog() if not omitted(command)]

    lines = [
        '"""Generated Praat menu commands. Built by codegen/generate.py."""',
        "",
        "from pypraat._pypraat import Object, invoke",
        "",
        "",
        "def _given(*values):",
        "    items = list(values)",
        "    while items and items[-1] is None:",
        "        items.pop()",
        "    if any(item is None for item in items):",
        "        raise ValueError('only trailing arguments can be omitted')",
        "    return items",
        "",
        "",
        "def _raw_of(obj):",
        "    raw = getattr(obj, '_raw', None)",
        "    return obj if raw is None else raw",
        "",
        "",
        "def _wrap(result):",
        "    if isinstance(result, Object):",
        "        return _BY_NAME.get(result.class_name, Thing)(result)",
        "    if isinstance(result, tuple):",
        "        return tuple(_wrap(item) for item in result)",
        "    return result",
        "",
        "",
        "_BY_NAME = {}",
        "",
    ]
    stub = [
        "from __future__ import annotations",
        "",
        "from typing import Any, Sequence",
        "",
        "class Object:",
        "    class_name: str",
        "    name: str",
        "",
        "def invoke(objects: Any, title: str, *args: Any) -> Any: ...",
        "def run_file(path: str) -> None: ...",
        "",
    ]
    methods = {name: [] for name in classes}
    functions = []
    used_functions = set()

    for command in commands:
        slots = object_slots(command)
        fields = field_names(command["fields"])
        title = command["title"]
        base = snake(title)
        if not slots:
            name = unique(base, used_functions)
            signature = _signature([], fields)
            call = _call(title, "[]", fields)
            functions.append((name, signature, call, title, []))
            continue
        if len(slots) == 1:
            # n == 1 is one object. n == 0 is Praat's "any number", which still
            # has a method on that one object; the sequence form is extra.
            if slots[0][2] in methods:
                methods[slots[0][2]].append((base, slots, fields, title, True))
            if slots[0][1]:
                name = unique(base, used_functions)
                signature = _signature([slots[0][0]], fields)
                call = _call(
                    title, f"[_raw_of(item) for item in {slots[0][0]}]", fields
                )
                functions.append((name, signature, call, title, [slots[0]]))
            elif slots[0][2] not in methods:
                name = unique(base, used_functions)
                signature = _signature([slots[0][0]], fields)
                call = _call(title, f"[_raw_of({slots[0][0]})]", fields)
                functions.append(
                    (name, signature, call, title, [(slots[0][0], False, slots[0][2])])
                )
            continue
        name = unique(base, used_functions)
        signature = _signature([slot[0] for slot in slots], fields)
        receivers = "[" + ", ".join(_receiver(slot) for slot in slots) + "]"
        functions.append(
            (name, signature, call := _call(title, receivers, fields), title, slots)
        )
        if not slots[0][1] and slots[0][2] in methods:
            methods[slots[0][2]].append((base, slots, fields, title, False))

    exported = []
    for name in classes:
        parent = parents[name] or "object"
        lines.append(f"class {name}({parent}):")
        stub.append(f"class {name}({parent}):")
        body = methods.get(name) or []
        if name == "Thing":
            lines.extend(
                [
                    "    def __init__(self, raw):",
                    "        if not isinstance(raw, Object):",
                    "            raise TypeError('expected a Praat object')",
                    "        self._raw = raw",
                    "",
                    "    @property",
                    "    def class_name(self):",
                    "        return self._raw.class_name",
                    "",
                    "    @property",
                    "    def name(self):",
                    "        return self._raw.name",
                    "",
                ]
            )
            stub.extend(
                [
                    "    def __init__(self, raw: Object) -> None: ...",
                    "    @property",
                    "    def class_name(self) -> str: ...",
                    "    @property",
                    "    def name(self) -> str: ...",
                ]
            )
        if not body and name != "Thing":
            lines.append("    pass")
            stub.append("    pass")
        used = set()
        for base, slots, fields, title, method_only in body:
            method = unique(base, used)
            others = [] if method_only else slots[1:]
            if method_only:
                others = []
            else:
                others = slots[1:]
            signature = _signature(
                [slot[0] for slot in others], fields, self_name="self"
            )
            if method_only:
                receivers = "[self._raw]"
            else:
                receivers = (
                    "[self._raw, " + ", ".join(_receiver(slot) for slot in others) + "]"
                )
            lines.append(f"    def {method}({signature}):")
            lines.append(f"        return {_call(title, receivers, fields)}")
            lines.append(f"    {method}._praat_command = {title!r}")
            lines.append("")
            stub_params = ["self", *[annotation(slot) for slot in others]]
            stub_params.extend(f"{field}: Any = None" for field in fields)
            stub.append(f"    def {method}({', '.join(stub_params)}) -> Any: ...")
        lines.append(f"_BY_NAME[{name!r}] = {name}")
        lines.append("")
        exported.append(name)

    for name, signature, call, title, slots in functions:
        lines.append(f"def {name}({signature}):")
        lines.append(f"    return {call}")
        lines.append(f"{name}._praat_command = {title!r}")
        lines.append("")
        stub_params = [annotation(slot) for slot in slots]
        field_part = [
            part.split("=", 1)[0] for part in signature.split(", ") if "=" in part
        ]
        stub_params.extend(f"{field}: Any = None" for field in field_part)
        stub.append(f"def {name}({', '.join(stub_params)}) -> Any: ...")
        exported.append(name)

    lines.append("__all__ = [" + ", ".join(repr(name) for name in exported) + "]")
    lines.append("")
    return "\n".join(lines), "\n".join(stub) + "\n"


def _receiver(slot):
    name, sequence, _class_name = slot
    if sequence:
        return f"[_raw_of(item) for item in {name}]"
    return f"_raw_of({name})"


def annotation(slot):
    name, sequence, class_name = slot
    if sequence:
        return f"{name}: Sequence[{class_name}]"
    return f"{name}: {class_name}"


def check():
    source, stub = render()
    current_source = COMMANDS.read_text(encoding="utf-8") if COMMANDS.exists() else ""
    current_stub = STUB.read_text(encoding="utf-8") if STUB.exists() else ""
    if source != current_source or stub != current_stub:
        raise SystemExit("generated Praat commands are stale; run codegen/generate.py")


def main():
    source, stub = render()
    if "--check" in sys.argv:
        check()
        return
    COMMANDS.write_text(source, encoding="utf-8")
    STUB.write_text(stub, encoding="utf-8")


if __name__ == "__main__":
    main()
