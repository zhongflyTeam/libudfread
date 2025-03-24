# Copyright (C) 2025 Rob Hall
# SPDX-License-Identifier: MIT

# libudfread source files
libudfread_src = [
    'default_blockinput.c',
    'ecma167.c',
    'udfread.c',
    'udfread-version.c',
]

#
# Library definitions
#

if host_machine.system() == 'windows' and get_option('default_library') == 'static'
    api_export_flags = []
else
    api_export_flags = '-DUDFREAD_API_EXPORT'
endif

# The final libudfread library
if get_option('staticlib_only')
    libudfread = static_library('udfread', libudfread_src,
        dependencies: threads,
        include_directories: inc_dirs,
        install: false)
else
    libudfread = library('udfread', libudfread_src,
        include_directories: libudfread_inc_dirs,
        dependencies: [thread_dependency],
        c_args: [api_export_flags],
        version: libudfread_soname_version,
        gnu_symbol_visibility: 'hidden',
        install: true)
endif

# Make the library usable as a dependency in this and other projects
libudfread_dep = declare_dependency(link_with: libudfread,
    include_directories: '.')

meson.override_dependency('libudfread', libudfread_dep)

# Generate pkg-config .pc file
if not get_option('staticlib_only')
    pkg_mod = import('pkgconfig')
    pkg_mod.generate(libraries: libudfread,
        version: meson.project_version(),
        name: 'libudfread',
        description: 'UDF filesystem access library')
endif
