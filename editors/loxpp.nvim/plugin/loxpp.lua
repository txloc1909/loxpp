-- Load guard and lazy entry point. This file stays cheap: it never calls
-- `require("loxpp")` at plugin-load time. The module is pulled in only when
-- the user calls `require("loxpp").setup()` or when the first `lox` buffer
-- appears.

if vim.g.loaded_loxpp then
  return
end
vim.g.loaded_loxpp = true

-- `:checkhealth loxpp` resolves `lua/loxpp/health.lua` on its own; nothing to
-- register here.

vim.api.nvim_create_autocmd("FileType", {
  group = vim.api.nvim_create_augroup("loxpp_bootstrap", { clear = true }),
  pattern = "lox",
  once = true,
  callback = function()
    local loxpp = require("loxpp")
    if not loxpp.did_setup then
      loxpp.setup()
    end
  end,
})
