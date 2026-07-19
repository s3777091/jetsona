#!/usr/bin/env python3
"""Regression guard for the terminal's latency-critical render architecture.

This is intentionally source-level: it runs on developer machines that cannot
build the Linux/forkpty firmware and catches the exact regressions that made the
Nano unusable (scrollback in the editable textarea, or a synchronous output
render on Enter).  The normal firmware build remains the authoritative API and
link check.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/display/views/terminal_view.cc").read_text(encoding="utf-8")
HEADER = (ROOT / "src/display/views/terminal_view.h").read_text(encoding="utf-8")


def function_body(signature: str) -> str:
    start = SOURCE.find(signature)
    assert start >= 0, f"missing function: {signature}"
    opening = SOURCE.find("{", start)
    assert opening >= 0, f"missing body: {signature}"
    depth = 0
    for index in range(opening, len(SOURCE)):
        char = SOURCE[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[opening + 1 : index]
    raise AssertionError(f"unterminated body: {signature}")


# Only the current command may be an editable textarea.  A second textarea is
# almost certainly scrollback being reintroduced into the caret/layout path.
assert SOURCE.count("lv_textarea_create(") == 1
assert "lv_textarea_create(input_row_)" in SOURCE
assert "lv_canvas_create(output_holder_)" in SOURCE
assert "lv_draw_label(" in function_body("void TerminalView::RenderOutput(")
assert "lv_textarea_set_text" not in function_body("void TerminalView::RenderOutput(")

# Enter must clear the short input and enqueue an output refresh; rasterizing
# 4 KB synchronously here recreates the visible key/Enter stall.
submit = function_body("void TerminalView::SubmitInput(")
assert 'lv_textarea_set_text(input_, "")' in submit
assert "dirty_.store(true)" in submit
assert "RenderOutput(" not in submit

# The split surfaces and owned canvas buffer are lifetime-significant members,
# not local objects that disappear after construction.
assert "lv_obj_t *output_canvas_" in HEADER
assert "lv_draw_buf_t *output_draw_buf_" in HEADER
assert "lv_obj_t *input_" in HEADER

print("terminal render-path regression checks passed")
