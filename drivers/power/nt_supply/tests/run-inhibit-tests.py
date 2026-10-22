#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run the production charger-class inhibit methods with a fake hardware driver."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
source = (HERE.parent / "charger_class.c").read_text()
start = source.index("int charger_dev_enable(")
end = source.index("int charger_dev_is_enabled(", start)
with tempfile.TemporaryDirectory(prefix="tetris-charge-inhibit-") as work:
    path = Path(work)
    generated = (HERE / "inhibit_test.c").read_text().replace(
        "/* PRODUCTION_METHODS */", source[start:end])
    (path / "test.c").write_text(generated)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, timeout=10)
