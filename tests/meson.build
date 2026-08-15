# Copyright (C) 2026 VLC authors and VideoLAN
# SPDX-License-Identifier: MIT

# Unit tests: ecma167.c internals.
test_ecma167 = executable('test_ecma167', 'test_ecma167.c',
    include_directories: '../src',
    c_args: ['-UHAVE_CONFIG_H', '-DHAVE_CONFIG_H=0'])

test('ecma167', test_ecma167,
    suite: 'unit',
    protocol: 'exitcode')

# Unit tests: udf_volume.c internals.
test_udf_volume = executable('test_udf_volume', 'test_udf_volume.c',
    include_directories: '../src',
    c_args: ['-UHAVE_CONFIG_H', '-DHAVE_CONFIG_H=0'])

test('udf_volume', test_udf_volume,
    suite: 'unit',
    protocol: 'exitcode')

# Coverage summary: runs the test binary and prints per-file line coverage.
# Requires the build to be configured with -Db_coverage=true.
run_target('coverage-summary',
    command: [files('coverage.sh'), meson.project_build_root()])