#!/usr/bin/env python3

import argparse
import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

GAME_HEADER_RE = re.compile(r"^  ([A-Za-z0-9_.+\-]+):\s*$")
FIELD_RE = re.compile(r"^    ([A-Za-z0-9_]+):\s*(.*?)\s*$")


@dataclass
class GameBlock:
    name: str
    start: int
    end: int
    parent_line: Optional[int]
    parent: str
    system: str
    title_line: Optional[int]
    archive_line: Optional[int]


def unquote_yaml_scalar(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in ('"', "'"):
        return value[1:-1]
    return value


def load_database(path: Path):
    raw = path.read_bytes()
    has_bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.splitlines(keepends=True)

    games_index = None
    for index, line in enumerate(lines):
        if line.rstrip("\r\n") == "games:":
            games_index = index
            break
    if games_index is None:
        raise ValueError("Database does not contain a top-level 'games:' map.")

    games_end = len(lines)
    for index in range(games_index + 1, len(lines)):
        stripped = lines[index].rstrip("\r\n")
        if stripped and not stripped.startswith((" ", "#")):
            games_end = index
            break

    headers = []
    for index in range(games_index + 1, games_end):
        match = GAME_HEADER_RE.match(lines[index].rstrip("\r\n"))
        if match:
            headers.append((index, match.group(1)))

    blocks = {}
    order = []
    for pos, (start, name) in enumerate(headers):
        end = headers[pos + 1][0] if pos + 1 < len(headers) else games_end
        parent_line = None
        parent = ""
        system = ""
        title_line = None
        archive_line = None

        for index in range(start + 1, end):
            match = FIELD_RE.match(lines[index].rstrip("\r\n"))
            if not match:
                continue
            key, value = match.groups()
            if key == "parent":
                parent_line = index
                parent = unquote_yaml_scalar(value)
            elif key == "system":
                system = unquote_yaml_scalar(value)
            elif key == "title":
                title_line = index
            elif key == "archive":
                archive_line = index

        blocks[name] = GameBlock(name, start, end, parent_line, parent, system, title_line, archive_line)
        order.append(name)

    return raw, text, lines, newline, has_bom, blocks, order


def parse_mame_stream(stream, wanted_names: Set[str]) -> Dict[str, str]:
    results = {}
    for event, elem in ET.iterparse(stream, events=("end",)):
        tag = elem.tag.rsplit("}", 1)[-1]
        if tag not in ("machine", "game"):
            continue
        name = elem.get("name")
        if name in wanted_names:
            results[name] = elem.get("cloneof") or ""
        elem.clear()
    return results


def load_mame_relationships(args, wanted_names: Set[str]) -> Dict[str, str]:
    if args.listxml:
        with Path(args.listxml).open("rb") as stream:
            return parse_mame_stream(stream, wanted_names)

    command = [args.mame, "-listxml"]
    with tempfile.TemporaryFile() as stderr_file:
        try:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=stderr_file)
        except OSError as exc:
            raise RuntimeError(f"Failed to start MAME executable '{args.mame}': {exc}") from exc

        try:
            if process.stdout is None:
                raise RuntimeError("MAME stdout pipe was not created.")
            relationships = parse_mame_stream(process.stdout, wanted_names)
        except Exception:
            process.kill()
            process.wait()
            raise
        finally:
            if process.stdout is not None:
                process.stdout.close()

        return_code = process.wait()
        if return_code != 0:
            stderr_file.seek(0)
            stderr_text = stderr_file.read().decode("utf-8", errors="replace").strip()
            detail = f"\n{stderr_text}" if stderr_text else ""
            raise RuntimeError(f"MAME -listxml failed with exit code {return_code}.{detail}")

    return relationships


def compare_relationships(blocks: Dict[str, GameBlock], order: List[str], mame: Dict[str, str]):
    mismatches = []
    unresolved = []

    for name in order:
        block = blocks[name]
        if name not in mame:
            unresolved.append(("not_in_mame", name, block.parent, ""))
            continue

        expected = mame[name]
        actual = block.parent
        if expected and expected not in blocks:
            unresolved.append(("parent_not_in_database", name, actual, expected))
            continue

        if actual == expected:
            continue
        if not actual and expected:
            kind = "missing_parent"
        elif actual and not expected:
            kind = "extra_parent"
        else:
            kind = "wrong_parent"
        mismatches.append((kind, name, actual, expected))

    return mismatches, unresolved


def print_report(blocks, order, mame, mismatches, unresolved):
    print(f"ArcadeDuck games:       {len(order)}")
    print(f"Matched in MAME:        {sum(1 for name in order if name in mame)}")
    print(f"Existing parent fields: {sum(1 for name in order if blocks[name].parent)}")
    print(f"Parent mismatches:      {len(mismatches)}")
    print(f"Unresolved entries:     {len(unresolved)}")

    if mismatches:
        print("\nParent mismatches:")
        for kind, name, actual, expected in mismatches:
            system = blocks[name].system or "<unknown>"
            print(f"  {kind:14} {name:16} system={system:24} db={actual or '-':16} mame={expected or '-'}")

    if unresolved:
        print("\nUnresolved entries:")
        for kind, name, actual, expected in unresolved:
            system = blocks[name].system or "<unknown>"
            if kind == "not_in_mame":
                print(f"  not_in_mame           {name:16} system={system}")
            else:
                print(f"  parent_not_in_database {name:16} system={system:24} mame_parent={expected}")


def update_database(path: Path, raw, lines, newline, has_bom, blocks, mismatches):
    replacements = {}
    removals = set()
    insertions = {}
    changed = 0

    for kind, name, actual, expected in mismatches:
        block = blocks[name]
        if kind == "extra_parent":
            removals.add(block.parent_line)
            changed += 1
            continue

        parent_line = f'    parent: "{expected}"{newline}'
        if block.parent_line is not None:
            replacements[block.parent_line] = parent_line
        else:
            anchor = block.title_line
            if anchor is None:
                anchor = block.archive_line
            if anchor is None:
                anchor = block.start
            insertions.setdefault(anchor, []).append(parent_line)
        changed += 1

    if changed == 0:
        return 0

    output = []
    for index, line in enumerate(lines):
        if index in removals:
            continue
        output.append(replacements.get(index, line))
        if index in insertions:
            output.extend(insertions[index])

    new_text = "".join(output)
    encoded = new_text.encode("utf-8")
    if has_bom:
        encoded = b"\xef\xbb\xbf" + encoded

    if encoded == raw:
        return 0

    temp_path = path.with_name(path.name + ".tmp")
    temp_path.write_bytes(encoded)
    os.replace(temp_path, path)
    return changed


def parse_args():
    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parent
    default_database = repo_root / "data" / "resources" / "arcadedb.yaml"

    parser = argparse.ArgumentParser(
        description="Audit and synchronize ArcadeDuck arcadedb.yaml parent relationships against MAME -listxml."
    )
    parser.add_argument(
        "--database",
        default=str(default_database),
        help="Path to arcadedb.yaml (default: repository data/resources/arcadedb.yaml).",
    )
    source = parser.add_mutually_exclusive_group()
    source.add_argument(
        "--mame",
        default="mame.exe" if sys.platform.startswith("win") else "mame",
        help="MAME executable used to run -listxml (default: mame.exe on Windows).",
    )
    source.add_argument(
        "--listxml",
        help="Use an existing MAME -listxml XML file instead of launching MAME.",
    )
    parser.add_argument(
        "--write",
        action="store_true",
        help="Update parent fields in arcadedb.yaml. Without this option the tool is audit-only.",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    database_path = Path(args.database).resolve()

    try:
        raw, text, lines, newline, has_bom, blocks, order = load_database(database_path)
        if not blocks:
            raise RuntimeError("No games were found in the ArcadeDuck database.")

        mame = load_mame_relationships(args, set(blocks))
        mismatches, unresolved = compare_relationships(blocks, order, mame)
        print_report(blocks, order, mame, mismatches, unresolved)

        if args.write:
            if unresolved:
                print("\nDatabase was not modified because unresolved MAME relationships require review.", file=sys.stderr)
                return 2
            changed = update_database(database_path, raw, lines, newline, has_bom, blocks, mismatches)
            if changed:
                print(f"\nUpdated {changed} parent relationship(s) in {database_path}")
            else:
                print("\nNo database changes were required.")
            return 0

        if mismatches or unresolved:
            print("\nAudit failed. Re-run with --write only after unresolved entries are addressed." if unresolved else
                  "\nAudit found parent mismatches. Re-run with --write to synchronize them.")
            return 2

        print("\nParent relationships match MAME.")
        return 0
    except (OSError, ValueError, RuntimeError, ET.ParseError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
