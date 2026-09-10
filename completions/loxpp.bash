# Bash completion for loxpp
# Source this file or install to /etc/bash_completion.d/ or
# ${XDG_DATA_HOME:-~/.local/share}/bash-completion/completions/loxpp

_loxpp_completion() {
    local cur prev
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    # Complete flags
    if [[ "$cur" == -* ]]; then
        COMPREPLY=($(compgen -W "--version --help --check --format" -- "$cur"))
        return 0
    fi

    # Complete --format values
    if [[ "$prev" == "--format" ]]; then
        COMPREPLY=($(compgen -W "text json" -- "$cur"))
        return 0
    fi

    # Complete filenames for --check
    if [[ "$prev" == "--check" ]]; then
        COMPREPLY=($(compgen -f -X '!*.lox' -- "$cur"))
        [[ ${#COMPREPLY[@]} -eq 0 ]] && COMPREPLY=($(compgen -f -- "$cur"))
        return 0
    fi

    # Default: complete filenames (for script argument)
    COMPREPLY=($(compgen -f -X '!*.lox' -- "$cur"))
    [[ ${#COMPREPLY[@]} -eq 0 ]] && COMPREPLY=($(compgen -f -- "$cur"))
    return 0
}

complete -o filenames -F _loxpp_completion loxpp
