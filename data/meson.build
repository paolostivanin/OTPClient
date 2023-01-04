# GSchema
gschema_conf = configuration_data()
gschema_conf.set('app-id', application_id)
gschema_conf.set('gettext-package', gettext_package)
configure_file(
    input: 'com.github.paolostivanin.OTPClient.gschema.xml.in',
    output: '@0@.gschema.xml'.format(application_id),
    configuration: gschema_conf,
    install: true,
    install_dir: datadir / 'glib-2.0' / 'schemas',
)

gnome.compile_schemas()
glib_compile_schemas = find_program('glib-compile-schemas', required: false)
# Validata GSchema
if glib_compile_schemas.found()
  test(
    'validate-gschema', glib_compile_schemas,
    args: [
      '--strict', '--dry-run', meson.current_source_dir()
    ],
  )
endif

# FreeDesktop Desktop File
desktop_conf = configuration_data()
desktop_conf.set('app-id', application_id)
desktop_file = i18n.merge_file(
  input: configure_file(
    input: 'com.github.paolostivanin.OTPClient.desktop.in.in',
    output: '@BASENAME@',
    configuration: desktop_conf
  ),
  output: '@0@.desktop'.format(application_id),
  po_dir: podir,
  type: 'desktop',
  install: true,
  install_dir: get_option('datadir') / 'applications',
)

# Validate Desktop File
desktop_file_validate = find_program('desktop-file-validate', required: false)
if desktop_file_validate.found()
  test (
    'Validate desktop file',
    desktop_file_validate,
    args: desktop_file.full_path(),
  )
endif

# Freedesktop AppData File
appdata_conf = configuration_data()
appdata_conf.set('app-id', application_id)
appdata_conf.set('gettext-package', gettext_package)
appdata_file = i18n.merge_file(
  input: configure_file(
    input: 'com.github.paolostivanin.OTPClient.appdata.xml.in.in',
    output: '@BASENAME@',
    configuration: appdata_conf
  ),
  output: '@0@.appdata.xml'.format(application_id),
  po_dir: podir,
  install: true,
  install_dir: get_option('datadir') / 'appdata',
)

# Validate appdata File
appstream_util = find_program('appstream-util', required: false)
if appstream_util.found()
  test (
    'Validate appdata file',
    appstream_util,
    args: ['validate-relax', '--nonet', appdata_file.full_path()],
  )
endif

