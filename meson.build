# SPDX-License-Identifier: BSD-3-Clause
project('vqda', 'c',
  version: '0.1.0',
  license: 'BSD-3-Clause AND GPL-2.0-only WITH Linux-syscall-note',
  license_files: ['LICENSES/BSD-3-Clause.txt', 'LICENSES/GPL-2.0-only.txt',
                  'LICENSES/Linux-syscall-note.txt'],
  meson_version: '>= 1.1',
  default_options: ['c_std=gnu11', 'warning_level=2',
                    'buildtype=debugoptimized'])

if get_option('optimization') not in ['0', 'g']
  add_project_arguments('-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=2',
    language: 'c')
endif

inc = include_directories('include')

test('proto', executable('test-proto', 'tests/test-proto.c',
  include_directories: inc))

threads = dependency('threads')

test('xs', executable('test-xs', 'tests/test-xs.c',
  include_directories: inc, dependencies: threads))
