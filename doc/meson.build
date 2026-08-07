# Copyright (C) 2025 Rob Hall
# SPDX-License-Identifier: MIT

if not get_option('enable_docs')
    subdir_done()
endif

doxygen = find_program('doxygen', required: true)

doxyfile = configure_file(
    input: 'Doxyfile.in',
    output: 'Doxyfile',
    configuration: {'INPUT': meson.project_build_root() / 'src/udfread',
                    'STRIP_FROM_PATH': meson.project_build_root() / 'src',
                    'OUTPUT_DIRECTORY': meson.current_build_dir(),
                    'PROJECT_VERSION': meson.project_version(),
                    'HTML_FOOTER': meson.current_source_dir() / 'doxygen-footer.html'})

custom_target('docs',
    depends: libudfread_internal_headers,
    command: [doxygen, doxyfile],
    output: ['html'],
    build_by_default: false,
    install: true,
    install_tag: 'doc',
    install_dir: get_option('datadir') / 'doc/libudfread')
