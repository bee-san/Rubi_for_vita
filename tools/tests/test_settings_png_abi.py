#!/usr/bin/env python3
"""Protect the native bounds gate against unsupported pointer arithmetic."""
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_settings_png_abi import jump_span


class NativeJumpBounds(unittest.TestCase):
    SAVE = ["mov ip, sp", "stmia.w r0!, {r4, r5, r6, r7, r8, r9, sl, fp, ip, lr}",
            "vstmia r0, {d8-d15}", "mov.w r0, #0", "bx lr"]
    RESTORE = ["ldmia.w r0!, {r4, r5, r6, r7, r8, r9, sl, fp, ip, lr}", "mov sp, ip",
               "vldmia r0, {d8-d15}", "movs r0, r1", "it eq", "moveq r0, #1", "bx lr"]

    def test_current_linked_arm_newlib_spans(self):
        self.assertEqual(jump_span(self.SAVE, True), 104)
        self.assertEqual(jump_span(self.RESTORE, False), 104)

    def test_pointer_adjustment_cannot_hide_a_larger_span(self):
        for instructions, store, insert in [(self.SAVE, True, 2), (self.RESTORE, False, 2)]:
            with self.subTest(store=store), self.assertRaises(ValueError):
                changed = instructions[:insert] + ["add r0, r0, #64"] + instructions[insert:]
                jump_span(changed, store)

    def test_direct_out_of_buffer_access_is_rejected(self):
        with self.assertRaises(ValueError):
            jump_span(self.SAVE[:2] + ["str r4, [r0, #160]"] + self.SAVE[2:], True)

    def test_transfer_after_return_value_cannot_reset_the_pointer(self):
        with self.assertRaises(ValueError):
            jump_span(self.SAVE[:-1] + ["vstmia r0, {d8-d15}", "bx lr"], True)

    def test_unknown_call_and_control_flow_cannot_hide_writes(self):
        for instruction in ["bl 1234 <helper>", "beq 1234 <alternate>"]:
            with self.subTest(instruction=instruction), self.assertRaises(ValueError):
                jump_span(self.SAVE[:2] + [instruction] + self.SAVE[2:], True)

    def test_missing_writeback_and_unknown_implementation_fail_closed(self):
        with self.assertRaises(ValueError):
            jump_span([line.replace("r0!", "r0") for line in self.SAVE], True)
        with self.assertRaises(ValueError):
            jump_span(["str r4, [r0]", "bx lr"], True)


if __name__ == "__main__":
    unittest.main()
