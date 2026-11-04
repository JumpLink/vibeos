# libspongy: Android's scrolling physics (the stretch overscroll, the fling, springs, interpolators) for any app
spongy_lib = shared_library('spongy-1', 'spongy.c',
  dependencies: libm,
  version: '0.1.0',
  install: true)
spongy_dep = declare_dependency(link_with: spongy_lib, include_directories: include_directories('.'))
install_headers('spongy.h', subdir: 'spongy-1')
import('pkgconfig').generate(spongy_lib, name: 'spongy-1', subdirs: 'spongy-1',
  description: 'Android stretch overscroll, fling and spring physics')

test('curves', executable('test-curves', 'tests/curves.c', dependencies: spongy_dep))
