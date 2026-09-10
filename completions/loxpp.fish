# Fish completion for loxpp

complete -c loxpp -f -d "Lox++ interpreter"

# Flags
complete -c loxpp -n "__fish_use_subcommand_from_list" -s h -l help -d "Show usage help"
complete -c loxpp -n "__fish_use_subcommand_from_list" -l version -d "Show version and bundled dependencies"
complete -c loxpp -n "__fish_use_subcommand_from_list" -l check -d "Perform static analysis without running"

# --format flag (only after --check)
complete -c loxpp -n "__fish_seen_subcommand_from check" -l format -x -d "Output format" -a "text json"

# Lox++ file completion
complete -c loxpp -n "__fish_seen_subcommand_from check" -f -a "*.lox" -d "Lox++ script"
complete -c loxpp -n "not __fish_seen_subcommand_from check; and not __fish_seen_subcommand_from help; and not __fish_seen_subcommand_from version" \
    -f -a "*.lox" -d "Lox++ script"
