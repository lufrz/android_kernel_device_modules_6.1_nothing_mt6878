#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Compile the production input-only policy with a deterministic hardware fixture."""
from pathlib import Path
import os
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
source = (HERE.parent / "mtk_basic_charger.c").read_text()
methods = "\n".join((
    source[source.index("static bool is_typec_adapter("):
           source.index("static bool support_fast_charging(")],
    source[source.index("static int select_source_current_limit("):
           source.index("static bool select_charging_current_limit(")],
    source[source.index("static int update_power_path_current("):
           source.index("static bool mtk_switch_check_charging_time(")],
))
with tempfile.TemporaryDirectory(prefix="tetris-power-path-") as work:
    path = Path(work)
    generated = (HERE / "power_path_test.c").read_text().replace(
        "/* PRODUCTION_METHODS */", methods)
    (path / "test.c").write_text(generated)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, timeout=10)
