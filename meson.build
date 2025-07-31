project(
  'xdg-dbus-proxy',
  'cpp',
  version : '0.1.6',
  meson_version : '>=0.49.0',
  default_options : [
    'cpp_std=c++20',
    'warning_level=2',
  ],
)

glib_dep = dependency('glib-2.0', version : '>=2.64')
gio_dep = dependency('gio-2.0', version : '>=2.66')
gio_unix_dep = dependency('gio-unix-2.0', required: true)
glibmm_dep = dependency('glibmm-2.4', version : '>=2.64')
giomm_dep = dependency('giomm-2.4', version : '>=2.66')

common_deps = [glib_dep, gio_dep, gio_unix_dep, glibmm_dep, giomm_dep]

sources = [
  'dbus-proxy.cpp',
  'source/flatpak-proxy-client.cpp',
  'source/buffer.cpp',
  'source/header.cpp',
  'source/filter.cpp',
  'source/proxyside.cpp',
  'source/utils.cpp',
]

headers = [
  'headers/flatpak-proxy-client.h',
  'headers/utils.h',
]

dbus_proxy = executable(
  'xdg-dbus-proxy',
  sources + headers,
  install : true,
  install_dir : get_option('bindir'),
  dependencies : common_deps,
  include_directories : include_directories('.'),
)