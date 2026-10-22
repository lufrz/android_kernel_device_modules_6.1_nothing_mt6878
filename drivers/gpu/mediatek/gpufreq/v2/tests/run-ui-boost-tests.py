#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Compile and test the actual UI boost helpers against fake GPU hardware.

Only the kernel API boundary is stubbed. Function bodies and state declarations
are extracted from the production sources on every run, so changing production
behavior changes these tests. No kernel build or target device is required.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent


def braced_item(source, pattern):
    """Extract a named C definition, excluding prototypes and trailing items."""
    match = re.search(pattern, source, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"Production definition not found: {pattern}")
    start = match.start()
    opening = source.index("{", match.start())
    # Ignore braces in comments and string/character literals.
    tokens = re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'
                         r"'(?:\\.|[^'\\])*'|[{}]", source[opening:], re.DOTALL)
    depth = 0
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:opening + token.end()]
    raise RuntimeError(f"Unclosed production definition: {pattern}")


def function(source, name):
    return braced_item(source, rf"^(?:static )?(?:int|ssize_t|void|bool) {name}\([^;{{]*\)\s*\{{")


def generate():
    wrapper = (HERE.parent / "gpufreq_v2.c").read_text()
    debug = (HERE.parent / "gpufreq_debug.c").read_text()
    header = (HERE.parent / "include/gpufreq_v2.h").read_text()
    types = []
    for name in ("gpufreq_return", "gpufreq_posdiv", "gpufreq_target", "gpuppm_reserved_idx",
                 "gpuppm_limiter"):
        types.append(braced_item(header, rf"^enum {name}\s*\{{") + ";")
    for name in ("gpufreq_opp_info", "gpuppm_limit_info"):
        types.append(braced_item(header, rf"^struct {name}\s*\{{") + ";")
    maximum = re.search(r"^#define GPUFREQ_MAX_OPP_NUM[^\n]*", header, re.MULTILINE)
    if maximum is None:
        raise RuntimeError("GPUFREQ_MAX_OPP_NUM is missing")
    types.append(maximum.group())
    state = re.findall(r"^static (?:DEFINE_MUTEX\(gpufreq_ui_boost_lock\)|"
                       r"(?:bool|int|unsigned int) g_(?:ui_boost|api_boost)[^;]*);",
                       wrapper, re.MULTILINE)
    if not state:
        raise RuntimeError("UI boost production state is missing")
    definitions = [function(wrapper, name) for name in (
        "gpufreq_validate_target", "gpufreq_ui_boost_oppidx",
        "gpufreq_ui_boost_debug_active", "gpufreq_ui_boost_init_locked",
        "gpufreq_ui_boost_limit_idx", "gpufreq_ui_boost_floor",
        "gpufreq_set_limit", "gpufreq_set_ui_boost_khz")]
    parsers = [function(debug, name) for name in (
        "gpufreq_parse_ui_boost", "limit_table_proc_write")]
    return (HERE / "ui_boost_test.c").read_text().replace(
        "/* PRODUCTION_TYPES */", "\n\n".join(types)).replace(
        "/* PRODUCTION_STATE */", "\n".join(state)).replace(
        "/* PRODUCTION_METHODS */", "\n\n".join(definitions)).replace(
        "/* PRODUCTION_PARSERS */", "\n\n".join(parsers))


def run(output, sanitizer):
    output.mkdir(parents=True, exist_ok=True)
    source = output / "ui_boost_test.generated.c"
    source.write_text(generate())
    executable = output / ("ui_boost_test-" + (sanitizer or "plain"))
    command = [os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
               "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare",
               "-pthread", "-g", "-O1"]
    if sanitizer:
        command += [f"-fsanitize={sanitizer}", "-fno-omit-frame-pointer"]
    subprocess.run(command + [str(source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=30)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="Keep generated source and binary")
    parser.add_argument("--sanitizer", default="address,undefined",
                        help="Compiler sanitizers (default: address,undefined; empty disables)")
    args = parser.parse_args()
    if args.output:
        run(args.output, args.sanitizer)
    else:
        with tempfile.TemporaryDirectory(prefix="gpufreq-ui-boost-") as work:
            run(Path(work), args.sanitizer)


if __name__ == "__main__":
    main()
