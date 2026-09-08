# nlohmann/json — vendored single header

`json.hpp` is the single-header build of *JSON for Modern C++*. The
`loxpp-lsp` language server uses it for JSON-RPC message parsing and building.
It is the first vendored third-party library in this repository.

## Version

| Field | Value |
|---|---|
| Release tag | `v3.12.0` |
| Released | 2025-04-11 |
| Upstream project | https://github.com/nlohmann/json |
| Downloaded from | https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp |
| `json.hpp` SHA-256 | `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` |

`LICENSE.MIT` is copied verbatim from
https://raw.githubusercontent.com/nlohmann/json/v3.12.0/LICENSE.MIT

## Rules for this copy

- `json.hpp` is copied **verbatim** from the upstream release. Do not
  hand-edit it. To update, download the new release asset, replace the file,
  record the new tag and SHA-256 here, and re-run the SHA-256 check.
- `third_party/` is exempt from `clang-format`, `clang-tidy`, and the
  `-Werror` warning sweep. The local `third_party/.clang-format` and
  `third_party/.clang-tidy` files turn both tools off for this tree, and the
  CI lint and static-analysis jobs already scope their file lists to `src/`
  and `test/`.

## License

MIT License. Full text in `LICENSE.MIT`:

```
MIT License

Copyright (c) 2013-2025 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
