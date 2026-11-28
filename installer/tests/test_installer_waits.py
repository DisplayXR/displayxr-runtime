#!/usr/bin/env python3
"""Static checks on the runtime installer's waits on other programs (#2841).

The interactive installer was found on the install step with Back, Next and
Cancel all disabled after its catalog components had been installed: the
install thread never finished, so the wizard never moved on. NSIS leaves that
state only when every section returns, so every wait on another program in an
install section must be bounded, and its "timeout" answer must be reported
instead of being taken as an exit code.

These checks read installer/DisplayXRInstaller.nsi, installer/Catalog.nsh and
src/xrt/targets/get/get_main.c as text. No NSIS and no Windows needed.

    python3 installer/tests/test_installer_waits.py
"""
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
INSTALLER = HERE.parent
ROOT = INSTALLER.parent
NSI = INSTALLER / "DisplayXRInstaller.nsi"
CATALOG = INSTALLER / "Catalog.nsh"
GET_MAIN = ROOT / "src" / "xrt" / "targets" / "get" / "get_main.c"

# Programs that download or can block on the network / WMI. A call to one of
# these from an nsExec without /TIMEOUT can hold the install page forever.
SLOW = re.compile(r"(dxr-get|\$DxrCatExe|\$R1 install|conv models fetch|powershell)", re.IGNORECASE)
NSEXEC = re.compile(r"^\s*nsExec::(Exec|ExecToLog|ExecToStack)\b(.*)$")


def nsexec_lines(path: Path):
    for no, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        if line.lstrip().startswith(";"):
            continue
        m = NSEXEC.match(line)
        if m:
            yield no, m.group(2)


def define(path: Path, name: str) -> int:
    text = path.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"^\s*!define\s+" + re.escape(name) + r"\s+(\d+)\s*$", text, re.MULTILINE)
    if not m:
        raise AssertionError(f"{name} is not defined in {path.name}")
    return int(m.group(1))


def c_define_ms(path: Path, name: str) -> int:
    text = path.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"#define\s+" + re.escape(name) + r"\s+\(([\du\s\*]+)\)", text)
    if not m:
        raise AssertionError(f"{name} is not defined in {path.name}")
    value = 1
    for factor in m.group(1).replace("u", "").split("*"):
        value *= int(factor)
    return value


class InstallerWaits(unittest.TestCase):
    def test_slow_calls_are_bounded(self):
        unbounded = []
        for path in (NSI, CATALOG):
            for no, rest in nsexec_lines(path):
                if SLOW.search(rest) and "/TIMEOUT=" not in rest:
                    unbounded.append(f"{path.name}:{no}: nsExec{rest.strip()[:100]}")
        self.assertEqual(unbounded, [], "nsExec calls that can block forever:\n" + "\n".join(unbounded))

    def test_dxr_get_timeout_is_reported(self):
        text = CATALOG.read_text(encoding="utf-8")
        for fn in ("DxrCatInstallOne", "DxrCatInstallDefault"):
            body = text.split(f"Function {fn}", 1)[1].split("FunctionEnd", 1)[0]
            self.assertIn("/TIMEOUT=${DXR_CAT_SILENCE_MS}", body, fn)
            self.assertIn('$R3 == "timeout"', body, f"{fn} must report nsExec's timeout answer")
            self.assertIn('$R3 == "error"', body, f"{fn} must report nsExec's error answer")
            self.assertIn("Call DxrStepLog", body, f"{fn} must record the step it is waiting on")

    def test_heartbeat_is_shorter_than_the_silence_limit(self):
        # dxr-get is silent while a component's /S installer runs; its heartbeat
        # is what keeps nsExec's no-output timeout from ending a live install.
        heartbeat = c_define_ms(GET_MAIN, "CHILD_HEARTBEAT_MS")
        silence = define(CATALOG, "DXR_CAT_SILENCE_MS")
        self.assertLess(heartbeat * 4, silence, (heartbeat, silence))
        self.assertIn("is still running (%u s)", GET_MAIN.read_text(encoding="utf-8"))

    def test_step_log_exists_and_is_removed(self):
        text = NSI.read_text(encoding="utf-8")
        self.assertIn("Function DxrStepLog", text)
        self.assertIn('Delete "$INSTDIR\\install-steps.log"', text.split('Section "Uninstall"', 1)[1])


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
