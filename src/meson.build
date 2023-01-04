otpclient_sources = [
    'cli/main.c',
    'cli/help.c',
    'cli/get-data.c',
]

gnome = import('gnome')

otpclient_sources += gnome.compile_resources(
  meson.project_name(),
  'otpclient.gresource.xml',
  gresource_bundle: true,
  install: true,
  install_dir: pkgdatadir,
)

executable(
  'otpclient',
  otpclient_sources,
  install: true,
)
