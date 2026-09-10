# Fish completion for loxpp

# Main flags.
complete -c loxpp -f -l help -d "Print usage help"
complete -c loxpp -f -l version -d "Print version and bundled dependencies"
complete -c loxpp -f -l check -d "Perform static analysis without running"

# --format flag (only after --check).
complete -c loxpp -n "__fish_seen_subcommand_from check" -l format -x -a "text json" -d "Output format"

# .lox file argument (in check mode or when no flag is given).
complete -c loxpp -n "__fish_seen_subcommand_from check" -f -a "*.lox" -d "Lox++ script"
complete -c loxpp -n "not __fish_seen_subcommand_from check; and not __fish_seen_subcommand_from help; and not __fish_seen_subcommand_from version" -f -a "*.lox" -d "Lox++ script"
