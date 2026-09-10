#compdef _loxpp loxpp

# Zsh completion for loxpp

_loxpp() {
    local -a opts
    opts=(
        "--version[Show version and bundled dependencies]"
        "--help[Show usage help]"
        "--check[Perform static analysis without running]"
        "--format[Output format for --check]:format:(text json)"
    )

    if [[ ${CURRENT}} -eq 2 ]]; then
        _arguments $opts
    fi

    # Complete filenames if the last arg was a flag that needs a filename
    if [[ ${words[-2]} == "--check" ]] || [[ ${CURRENT}} -gt 2 && ${words[2]} != -* ]]; then
        _files -g "*.lox"
    else
        _arguments $opts
    fi
}

_loxpp "$@"
