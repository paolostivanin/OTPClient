project('OTPClient', 'c',
  version: '3.99.1',
  license: ['GPL-3.0'],
  meson_version: '>= 0.59.0',
  default_options: [ 'warning_level=2', ],
)

dependency('gtk4', version: '>= 4.10.0')
dependency('libadwaita-1')
dependency('glib-2.0', version: '>=2.74')
dependency('gio-2.0', version: '>= 2.74')
dependency('jansson', version: '>=2.14')
dependency('libgcrypt', version: '>=1.8.0')
dependency('libpng', version: '>=1.6.0')
dependency('zbar', version: '>=0.20.0')
dependency('uuid', version: '>=2.34.0')
dependency('libprotobuf-c', version: '>=1.4.0')
dependency('protobuf', version: '>=3.20.0')
dependency('libsecret-1', version: '>=0.20.0')
dependency('libqrencode', version: '>=4.1.0')
dependency('cotp', version: '>=2.0.0')

i18n = import('i18n')
gnome = import('gnome')

application_id = 'com.github.paolostivanin.OTPClient'

prefix = get_option('prefix')
bindir = prefix / get_option('bindir')
libdir = prefix / get_option('libdir')
localedir = prefix / get_option('localedir')
datadir = prefix / get_option('datadir')
pkgdatadir = datadir / meson.project_name()
iconsdir = datadir / 'icons'
podir = meson.project_source_root () / 'po'
gettext_package = meson.project_name()

config_h = configuration_data()
config_h.set_quoted('APPLICATION_ID', application_id)
config_h.set_quoted('GETTEXT_PACKAGE', gettext_package)
config_h.set_quoted('LOCALEDIR', localedir)
config_h.set_quoted('PACKAGE_VERSION', meson.project_version())
configure_file(
  output: 'otpclient-config.h',
  configuration: config_h,
)

#if get_option('is_flatpak') == true
#endif

subdir('data')
subdir('src')
subdir('po')

gnome.post_install(
  glib_compile_schemas: true,
  gtk_update_icon_cache: true,
  update_desktop_database: true,
)

summary({
    'prefix': prefix,
    'libdir': libdir,
    'datadir': datadir,
    'bindir': bindir,
    'localedir': localedir,
  },
  section: 'Directories',
)

