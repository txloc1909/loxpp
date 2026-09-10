#compdef loxpp

# Zsh completion for loxpp

_loxpp() {
    local -a opts
    opts=(
        "--version[Print version and bundled dependencies]"
        "--help[Print usage help]"
        "--check[Perform static analysis without running]"
        "--format[Output format for --check]:format:(text json)"
    )

    _arguments "$opts[@]" "*:file:_files -g '*.lox'"
}

_loxpp "$@"
