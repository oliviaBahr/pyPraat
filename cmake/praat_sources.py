"""Emit CMake source lists from Praat's own Makefiles.

Praat's top Makefile names each library directory. Each directory Makefile
names the object files that go into its archive. This script follows those
lists instead of globbing the tree.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


SKIP_DIRS = {"main"}
EXTENSIONS = (".cpp", ".c", ".cc", ".cxx", ".mm", ".m")


def join_continuations(text: str) -> list[str]:
    lines: list[str] = []
    pending = ""
    for raw in text.splitlines():
        if "#" in raw:
            raw = raw[: raw.index("#")]
        stripped = raw.rstrip()
        if stripped.endswith("\\"):
            pending += stripped[:-1] + " "
            continue
        lines.append(pending + stripped)
        pending = ""
    if pending:
        lines.append(pending)
    return lines


def expand(text: str, variables: dict[str, str], depth: int = 0) -> str:
    if depth > 30:
        raise SystemExit(f"makefile variable recursion in: {text}")

    def repl(match: re.Match[str]) -> str:
        name = match.group(1)
        if name not in variables:
            return match.group(0)
        return expand(variables[name], variables, depth + 1)

    return re.sub(r"\$\(([A-Za-z0-9_]+)\)", repl, text)


def parse_makefile(path: Path) -> tuple[list[str], list[str]]:
    variables: dict[str, str] = {}
    lines = join_continuations(path.read_text(encoding="utf-8", errors="replace"))
    assign = re.compile(r"^([A-Za-z0-9_]+)\s*(\+?=)\s*(.*)$")
    for line in lines:
        match = assign.match(line.strip())
        if not match:
            continue
        name, op, value = match.group(1), match.group(2), match.group(3)
        value = expand(value, variables).strip()
        if op == "+=":
            previous = variables.get(name, "")
            variables[name] = (previous + " " + value).strip()
        else:
            variables[name] = value

    objects: list[str] = []
    archive = re.compile(r"\$\(AR\)\s+cq\s+\S+\s+(.+)$")
    for line in lines:
        match = archive.search(line.strip())
        if not match:
            continue
        expanded = expand(match.group(1), variables)
        objects = [token for token in expanded.split() if token.endswith(".o")]

    includes: list[str] = []
    flags = variables.get("CPPFLAGS", "").split()
    index = 0
    while index < len(flags):
        token = flags[index]
        if token == "-I" and index + 1 < len(flags):
            includes.append(flags[index + 1])
            index += 2
            continue
        if token.startswith("-I") and token != "-I":
            includes.append(token[2:])
        index += 1
    return objects, includes


def library_dirs(praat: Path) -> list[str]:
    text = (praat / "Makefile").read_text(encoding="utf-8", errors="replace")
    dirs: list[str] = []
    for match in re.finditer(r"\$\(MAKE\)\s+-C\s+(\S+)", text):
        directory = match.group(1)
        if directory in SKIP_DIRS or directory.startswith("test/") or directory == "dwtest":
            continue
        if directory not in dirs:
            dirs.append(directory)
    return dirs


def cmake_escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def group_name(directory: str) -> str:
    return re.sub(r"[^A-Za-z0-9]", "_", directory)


def resolve_source(base: Path, object_name: str) -> Path:
    stem = object_name[:-2]
    for extension in EXTENSIONS:
        candidate = base / f"{stem}{extension}"
        if candidate.is_file():
            return candidate
    raise SystemExit(f"no source for {base / object_name}")


def main() -> None:
    praat = Path(sys.argv[1]).resolve()
    groups: list[str] = []
    chunks: list[str] = []
    total = 0
    for directory in library_dirs(praat):
        makefile = praat / directory / "Makefile"
        if not makefile.is_file():
            raise SystemExit(f"missing {makefile}")
        objects, includes = parse_makefile(makefile)
        if not objects:
            raise SystemExit(f"{makefile} produced no objects")
        sources = [resolve_source(praat / directory, obj) for obj in objects]
        include_paths = []
        for include in includes:
            include_paths.append((praat / directory / include).resolve())
        name = group_name(directory)
        groups.append(name)
        source_lines = "\n".join(f'  "{cmake_escape(str(path))}"' for path in sources)
        include_lines = "\n".join(f'  "{cmake_escape(str(path))}"' for path in include_paths)
        chunks.append(
            f"set(PRAAT_GROUP_{name}_SOURCES\n{source_lines}\n)\n"
            f"set(PRAAT_GROUP_{name}_INCLUDES\n{include_lines}\n)\n"
        )
        total += len(sources)
    chunks.append("set(PRAAT_GROUPS\n" + "\n".join(f"  {name}" for name in groups) + "\n)\n")
    sys.stdout.write("".join(chunks))
    print(f"# {total} sources in {len(groups)} libraries", file=sys.stderr)


if __name__ == "__main__":
    main()
