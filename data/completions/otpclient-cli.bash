# bash completion for otpclient-cli
# Install to /usr/share/bash-completion/completions/otpclient-cli

_otpclient_cli() {
    local cur prev opts types formats
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    opts="--help --help-all --version -v \
          --show -s --account -a --issuer -i --match-exact -m --show-next -n \
          --list -l --list-databases --list-types \
          --import --export --type -t --file -f \
          --output-dir -o --password-file -p --output \
          --export-settings --import-settings --database -d"

    types="aegis_plain aegis_encrypted authpro_plain authpro_encrypted \
           twofas_plain twofas_encrypted freeotpplus_plain"
    formats="table json csv"

    case "$prev" in
        -t|--type)
            COMPREPLY=( $(compgen -W "$types" -- "$cur") )
            return 0
            ;;
        -f|--file|-p|--password-file)
            COMPREPLY=( $(compgen -f -- "$cur") )
            return 0
            ;;
        -o|--output-dir|-d|--database)
            # --database also accepts a name; fall back to file/dir completion.
            COMPREPLY=( $(compgen -f -- "$cur") )
            return 0
            ;;
        --output)
            COMPREPLY=( $(compgen -W "$formats" -- "$cur") )
            return 0
            ;;
    esac

    # Handle --output=<TAB>
    case "$cur" in
        --output=*)
            COMPREPLY=( $(compgen -W "$formats" -- "${cur#--output=}") )
            return 0
            ;;
        -*)
            COMPREPLY=( $(compgen -W "$opts" -- "$cur") )
            return 0
            ;;
    esac

    COMPREPLY=( $(compgen -W "$opts" -- "$cur") )
}

complete -F _otpclient_cli otpclient-cli
