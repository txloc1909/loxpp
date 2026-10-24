#!/usr/bin/env python3
"""Check editors/loxpp.nvim/syntax/lox.vim against the spec probe token forms.

The Neovim fallback syntax is a regex highlighter, so no tree-sitter parser
sees a defect in it. This script runs a real headless Neovim with `syntax on`,
sources the fallback file, and reads the scope of the literal in each form
that probe 165, 242, and 244 pin (spec/01-lexical.md).

Every case names one token and the scope that the syntax must give it. A
literal that a keyword or a name follows directly must still be one token:
`1iin` is `INT 1i` then `in`, not `NUMBER 1` then a name.
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SYNTAX = ROOT / "editors" / "loxpp.nvim" / "syntax" / "lox.vim"

# (case name, source line, zero-based byte index in the literal, want scope)
CASES = [
    ("int_suffix_before_in", "1iin [1i];", 0, "loxNumber"),
    ("int_suffix_before_and", "1iand true;", 0, "loxNumber"),
    ("hex_before_or", "0x1or false;", 0, "loxNumber"),
    ("int_guard_suffix_before_if", "case 1iif t", 5, "loxNumber"),
    ("fractional_suffix_before_in", "1.5in [1.5];", 0, "loxNumberError"),
    ("fractional_suffix_before_if", "case 1.5if t", 5, "loxNumberError"),
    ("hex_suffix_is_a_name", "0xFFi", 0, "loxNumber"),
    ("binary_suffix_is_a_name", "0b1i", 0, "loxNumber"),
    ("uppercase_hex_prefix_is_number_zero", "0XFF", 0, "loxNumber"),
    ("hex_literal", "0xFF", 0, "loxNumber"),
    ("binary_literal", "0b1010", 0, "loxNumber"),
    ("decimal_int", "1i", 0, "loxNumber"),
    ("fractional_literal", "1.5", 0, "loxNumber"),
    ("fractional_suffix_alone", "1.5i", 0, "loxNumberError"),
]

LUA_DRIVER = r"""
local cases = {CASES}
local failed = false
local out = {}
for _, c in ipairs(cases) do
  vim.api.nvim_buf_set_lines(0, 0, -1, false, { c.text })
  local name = vim.fn.synIDattr(vim.fn.synID(1, c.idx + 1, 1), "name")
  if name == c.want then
    table.insert(out, string.format("  ok  %-38s %s", c.name, name))
  else
    failed = true
    table.insert(out, string.format(
      "  FAIL %-37s got %q, want %q  (%s)",
      c.name, name, c.want, c.text))
  end
end
vim.fn.writefile(out, vim.env.LOX_SYNTAX_OUT)
if failed then
  vim.cmd("cq")
else
  vim.cmd("qa!")
end
"""


def lua_cases():
    rows = []
    for name, text, idx, want in CASES:
        rows.append(
            '  {name="%s", text="%s", idx=%d, want="%s"},'
            % (name, text.replace("\\", "\\\\").replace('"', '\\"'), idx, want)
        )
    return "{\n" + "\n".join(rows) + "\n}"


def main():
    nvim = os.environ.get("NVIM") or shutil.which("nvim")
    if not nvim:
        print("FAIL: nvim is not on PATH")
        return 1
    if not SYNTAX.is_file():
        print(f"FAIL: missing {SYNTAX.relative_to(ROOT)}")
        return 1

    with tempfile.TemporaryDirectory() as work:
        driver = Path(work) / "driver.lua"
        out_path = Path(work) / "out.txt"
        driver.write_text(
            "vim.cmd('syntax on')\n"
            f"vim.cmd('source {SYNTAX}')\n"
            + LUA_DRIVER.replace("{CASES}", lua_cases())
        )
        env = dict(os.environ, LOX_SYNTAX_OUT=str(out_path))
        proc = subprocess.run(
            [nvim, "--headless", "-u", "NONE", "-l", str(driver)],
            text=True,
            capture_output=True,
            env=env,
        )
        output = out_path.read_text() if out_path.is_file() else ""
        sys.stdout.write(output)
        if proc.stdout.strip():
            sys.stdout.write(proc.stdout)
        if proc.stderr.strip():
            sys.stderr.write(proc.stderr)
        if proc.returncode != 0 or "FAIL" in output:
            print("FAIL: the Neovim fallback syntax does not scope every token")
            return 1

    print(f"OK: {len(CASES)} Neovim fallback syntax scopes are correct")
    return 0


if __name__ == "__main__":
    sys.exit(main())
