# fish completion for otpclient-cli
# Install to /usr/share/fish/vendor_completions.d/otpclient-cli.fish

set -l otpclient_types aegis_plain aegis_enc authpro_plain authpro_enc twofas_plain twofas_enc freeotpplus_plain google_qr_file
set -l otpclient_formats table json csv

# Actions
complete -c otpclient-cli -l show -s s -d 'Show OTP for an account/issuer'
complete -c otpclient-cli -l list -s l -d 'List all tokens in the database'
complete -c otpclient-cli -l list-databases -d 'List known databases from the configuration'
complete -c otpclient-cli -l list-types -d 'List supported import/export types'
complete -c otpclient-cli -l import -d 'Import tokens from a backup file'
complete -c otpclient-cli -l export -d 'Export tokens to a backup file'
complete -c otpclient-cli -l export-settings -d 'Export app settings as JSON'
complete -c otpclient-cli -l import-settings -d 'Import app settings from a JSON file'
complete -c otpclient-cli -l version -s v -d 'Print version and exit'

# Selectors
complete -c otpclient-cli -l database -s d -r -d 'Database path or name'
complete -c otpclient-cli -l account -s a -r -d 'Account label'
complete -c otpclient-cli -l issuer -s i -r -d 'Issuer'
complete -c otpclient-cli -l match-exact -s m -d 'Case-sensitive equality match'
complete -c otpclient-cli -l show-next -s n -d 'Also print the next OTP'

# I/O and formatting
complete -c otpclient-cli -l type -s t -x -a "$otpclient_types" -d 'Backup format identifier'
complete -c otpclient-cli -l file -s f -r -d 'Backup or settings file'
complete -c otpclient-cli -l output-dir -s o -r -d 'Export destination directory'
complete -c otpclient-cli -l password-file -s p -r -d 'File containing the DB password'
complete -c otpclient-cli -l output -x -a "$otpclient_formats" -d 'Output format (table/json/csv)'
