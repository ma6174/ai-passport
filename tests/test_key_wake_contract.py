#!/usr/bin/env python3
"""Contracts for the deep-sleep key-wake guard.

The guard only matters when an application enters deep sleep and wakes on a GPIO:
the waking key is still held, so the button component grades it as a long press.
This check keeps the guard honest without pinning it to one application: if a
source file arms the guard, it must also consume it, and the guard state machine
must stay unit-tested.
"""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def main_sources() -> list[Path]:
    return sorted((ROOT / "main").glob("*.c"))


class KeyWakeGuardWiringTest(unittest.TestCase):
    def test_guard_is_consumed_wherever_it_is_armed(self) -> None:
        armed = [path for path in main_sources() if "key_guard_arm(" in path.read_text(encoding="utf-8")]
        for path in armed:
            source = path.read_text(encoding="utf-8")
            self.assertIn(
                "key_guard_consume(",
                source,
                f"{path.relative_to(ROOT)} arms the key guard without consuming it",
            )

    def test_guard_state_machine_has_a_host_test(self) -> None:
        self.assertTrue((ROOT / "tests" / "test_key_guard.c").is_file())
        self.assertTrue((ROOT / "main" / "key_guard.h").is_file())


if __name__ == "__main__":
    unittest.main()
