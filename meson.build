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

# vqda_build.h: VQDA_BUILD_ID, refreshed on every build from git describe
build_id = vcs_tag(input: 'include/vqda_build.h.in', output: 'vqda_build.h',
  command: ['git', '-C', meson.project_source_root(), 'describe',
            '--always', '--dirty'],
  fallback: meson.project_version())
build_id_args = ['-include', 'vqda_build.h']
# sanitizer runtimes do not link statically
static_args = (get_option('static') and get_option('b_sanitize') == 'none'
  ? ['-static'] : [])

executable('vqda-back', 'src/vqda-back.c', build_id,
  include_directories: inc, dependencies: threads, c_args: build_id_args,
  link_args: static_args,
  install: true)

test('back', executable('test-back', 'tests/test-back.c',
  include_directories: inc, dependencies: threads), timeout: 300,
  env: {'TSAN_OPTIONS': 'suppressions=' + meson.project_source_root()
                        / 'tests/tsan.supp'})

dl = meson.get_compiler('c').find_library('dl')
libvqda = shared_library('vqda', 'src/vqda-guest.c', build_id,
  include_directories: inc, dependencies: [threads, dl],
  c_args: build_id_args, install: true)

test('guest', executable('test-guest', 'tests/test-guest.c',
  include_directories: inc, link_with: libvqda))

# TSan's own start-up maps memory through the mmap this test defines
if get_option('b_sanitize') != 'thread'
  test('life', executable('test-life', 'tests/test-life.c',
    include_directories: inc, dependencies: [threads, dl]))
endif

foreach t : ['vqda-ping', 'vqda-neg', 'vqda-fuzz']
  executable(t, 'tools/' + t + '.c', include_directories: inc,
    link_with: libvqda, install: true)
endforeach
