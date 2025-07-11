project(
  'xdg-dbus-proxy',
  ['c','cpp'],
  version : '0.1.6',
  meson_version : '>=0.49.0',
  default_options : [
    'warning_level=2',
    'cpp_std=c++20',
  ],
)

cc = meson.get_compiler('c')
add_project_arguments('-D_GNU_SOURCE', language : 'c')
common_include_directories = include_directories('.')

add_project_arguments(
  cc.get_supported_arguments([
    '-Werror=aggregate-return',
    '-Werror=empty-body',
    '-Werror=implicit-function-declaration',
    '-Werror=incompatible-pointer-types',
    '-Werror=init-self',
    '-Werror=int-conversion',
    '-Werror=misleading-indentation',
    '-Werror=missing-declarations',
    '-Werror=missing-include-dirs',
    '-Werror=missing-prototypes',
    '-Werror=overflow',
    '-Werror=parenthesis',
    '-Werror=pointer-arith',
    '-Werror=return-type',
    '-Werror=shadow',
    '-Werror=strict-prototypes',
    '-Werror=switch-default',
    '-Wswitch-enum',

    # Deliberately not warning about this, ability to zero-initialize
    # a struct is a feature
    '-Wno-missing-field-initializers',
    '-Wno-error=missing-field-initializers',

    # Deliberately not warning about this, unused parameters to a callback
    # are not a bug
    '-Wno-unused-parameter',
    '-Wno-error=unused-parameter',
  ]),
  language : 'c',
)

glib_dep = dependency('glib-2.0', version : '>=2.40')
gio_dep = dependency('gio-2.0', version : '>=2.40')
gio_unix_dep = dependency('gio-unix-2.0', version : '>=2.40')
glibmm_dep = dependency('glibmm-2.4', required: true)
giomm_dep = dependency('giomm-2.4', required: true)
common_deps = [glib_dep, gio_dep, gio_unix_dep, glibmm_dep,giomm_dep]

if meson.is_subproject()
  bindir = get_option('libexecdir')

  if get_option('program_prefix') == ''
    error('program_prefix option must be set when xdg-dbus-proxy is a subproject')
  endif

  exe_name = get_option('program_prefix') + 'dbus-proxy'
else
  bindir = get_option('bindir')
  exe_name = 'xdg-dbus-proxy'
endif

conf_data = configuration_data()
conf_data.set_quoted('BINDIR', get_option('prefix') / bindir)
conf_data.set_quoted('PACKAGE_NAME', meson.project_name())
conf_data.set_quoted('PACKAGE_VERSION', meson.project_version())
configure_file(output : 'config.h',
               configuration : conf_data)

dbus_proxy = executable(
  exe_name,
  [
    'dbus-proxy.c',
    'flatpak-proxy.c',
    'dbus-proxy.cpp',
    'source/flatpak-proxy-client.cpp',
    'headers/flatpak-proxy-client.h',
  ],
  install : true,
  install_dir : bindir,
  dependencies : common_deps,
)

xsltproc = find_program('xsltproc', required : get_option('man'))

if xsltproc.found() and not meson.is_subproject()
  custom_target(
    'xdg-dbus-proxy.1',
    output : 'xdg-dbus-proxy.1',
    input : 'xdg-dbus-proxy.xml',
    command : [
      xsltproc,
      '--nonet',
      '--stringparam', 'man.output.quietly', '1',
      '--stringparam', 'funcsynopsis.style', 'ansi',
      '--stringparam', 'man.th.extra1.suppress', '1',
      '--stringparam', 'man.authors.section.enabled', '0',
      '--stringparam', 'man.copyright.section.enabled', '0',
      '-o', '@OUTPUT@',
      'http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl',
      '@INPUT@',
    ],
    install : true,
    install_dir : get_option('mandir') / 'man1',
  )
endif

if get_option('tests')
  subdir('tests')
endif
