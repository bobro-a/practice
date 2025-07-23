project(
  'xdg-dbus-proxy',
  'cpp',
  version : '0.1.6',
  meson_version : '>=0.49.0',
  default_options : [
    'cpp_std=c++20',
  ],
)

glib_dep = dependency('glibmm-2.4', version : '>=2.64')
gio_dep = dependency('giomm-2.4', version : '>=2.66')
gio_unix_dep = dependency('gio-unix-2.0', required: true)
common_deps = [glib_dep, gio_dep,gio_unix_dep]

dbus_proxy = executable(
  'xdg-dbus-proxy',
  [
    'dbus-proxy.cpp',
    'source/flatpak-proxy-client.cpp',
    'source/buffer.cpp',
    'source/header.cpp',
    'source/filter.cpp',
    'source/proxyside.cpp',
    'headers/flatpak-proxy-client.h',
    'headers/utils.h',
    'source/utils.cpp',
  ],
  install : true,
  install_dir : get_option('bindir'),
  dependencies : common_deps,
)