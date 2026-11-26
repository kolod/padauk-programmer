# SPDX-License-Identifier: GPL-3.0-or-later
"""pdkprog accepts the easypdkprog argument order: the chip before the command (FR-030)."""
from padauk_prog.cli import easypdkprog_order

COMMANDS = {"write", "read", "erase", "list"}


def test_chip_before_the_command_moves_after_it():
    assert easypdkprog_order(["-n", "PFS154", "write", "f.ihx"], COMMANDS) == ["write", "-n", "PFS154", "f.ihx"]
    assert easypdkprog_order(["-p", "COM3", "--icname=PMS150C", "read", "x.hex"], COMMANDS) == \
        ["-p", "COM3", "read", "--icname=PMS150C", "x.hex"]
    assert easypdkprog_order(["-nPFS154", "erase"], COMMANDS) == ["erase", "-nPFS154"]


def test_the_usual_order_is_kept():
    argv = ["-p", "tcp://127.0.0.1:1", "write", "-n", "PFS154", "f.ihx"]
    assert easypdkprog_order(argv, COMMANDS) == argv
    assert easypdkprog_order(["list"], COMMANDS) == ["list"]
