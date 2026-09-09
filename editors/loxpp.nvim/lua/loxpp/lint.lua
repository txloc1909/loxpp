-- nvim-lint linter spec: `loxpp --check --format json <file>`.
--
-- N3's JSON is an array of LSP-shaped, 0-based objects:
--   { line, character, endLine, endCharacter, severity, message }
-- which maps straight onto `vim.diagnostic` items (also 0-based).
--
-- This linter is registered by `require("loxpp").setup()` and runs only for a
-- `lox` buffer with no loxpp-lsp client attached.

local severity = {
  error = vim.diagnostic.severity.ERROR,
  warning = vim.diagnostic.severity.WARN,
  info = vim.diagnostic.severity.INFO,
  hint = vim.diagnostic.severity.HINT,
}

return {
  cmd = "loxpp",
  args = { "--check", "--format", "json" },
  stdin = false,
  append_fname = true,
  stream = "stdout",
  -- `loxpp --check` exits 1 when it finds an error; that is not a linter
  -- failure.
  ignore_exitcode = true,
  parser = function(output, _bufnr)
    local diagnostics = {}
    if not output or output == "" then
      return diagnostics
    end
    local ok, decoded = pcall(vim.json.decode, output)
    if not ok or type(decoded) ~= "table" then
      return diagnostics
    end
    for _, d in ipairs(decoded) do
      table.insert(diagnostics, {
        lnum = d.line or 0,
        col = d.character or 0,
        end_lnum = d.endLine or d.line or 0,
        end_col = d.endCharacter or d.character or 0,
        severity = severity[d.severity] or vim.diagnostic.severity.ERROR,
        message = d.message or "",
        source = "loxpp --check",
      })
    end
    return diagnostics
  end,
}
