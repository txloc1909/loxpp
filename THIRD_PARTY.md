# Third-party code

This file records every vendored third-party dependency in Lox++. Each entry
lists the component name, copyright holder, license, and the path to the
license file in this repository.

## Vendored dependencies

| Component | Copyright | License | License file |
|---|---|---|---|
| nlohmann/json | Niels Lohmann | MIT | `third_party/nlohmann/LICENSE.MIT` |
| isocline | Daan Leijen | MIT | `third_party/isocline/LICENSE` |
| isocline `wcwidth.c` | Markus Kuhn | Public domain / MIT-compatible | `third_party/isocline/src/wcwidth.c` (see header) |

## Vendoring rules

All vendored code in `third_party/` is copied **verbatim** from the upstream
source. Do not edit, reformat, or "fix" a vendored file. To update a
dependency, download the new upstream release, verify its checksum, extract,
replace the vendored tree, and record the new version and checksum in the
dependency's `README.md` file.

Vendored code is exempt from `clang-format`, `clang-tidy`, and the `-Werror`
warning sweep. The local `third_party/.clang-format` and `third_party/.clang-tidy`
configuration files enforce this for the entire tree.

See each dependency's `third_party/<name>/README.md` for upstream source URLs,
version pinning, file lists, and per-file license notices.
