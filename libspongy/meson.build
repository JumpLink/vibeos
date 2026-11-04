# libspongy: Android's scrolling physics (the stretch overscroll, the fling, springs, interpolators) for any app
pkg = import('pkgconfig')
spongy_lib = shared_library('spongy-1', 'spongy.c',
  dependencies: libm,
  version: '0.1.0',
  install: true)
spongy_dep = declare_dependency(link_with: spongy_lib, include_directories: include_directories('.'))
install_headers('spongy.h', subdir: 'spongy-1')
pkg.generate(spongy_lib, name: 'spongy-1', subdirs: 'spongy-1',
  description: 'Android stretch overscroll, fling and spring physics')

test('curves', executable('test-curves', 'tests/curves.c', dependencies: spongy_dep))

# spongy-gtk: the same as a GTK 4 widget (SpongyScrollView), and its introspection data for GJS and Python
gtk4 = dependency('gtk4', version: '>= 4.14', required: false)
if gtk4.found()
  spongy_gtk_lib = shared_library('spongy-gtk-1', 'spongy-gtk.c',
    dependencies: [gtk4, libm, spongy_dep],
    version: '0.1.0',
    install: true)
  install_headers('spongy-gtk.h', subdir: 'spongy-1')
  pkg.generate(spongy_gtk_lib, name: 'spongy-gtk-1', subdirs: 'spongy-1', requires: ['gtk4', 'spongy-1'],
    description: 'A GTK 4 scroll view with Android\'s stretch overscroll and fling')
  if find_program('g-ir-scanner', required: false).found()
    import('gnome').generate_gir(spongy_gtk_lib,
      sources: ['spongy-gtk.h', 'spongy-gtk.c'],
      namespace: 'Spongy', nsversion: '1',
      symbol_prefix: 'spongy', identifier_prefix: 'Spongy',
      includes: ['Gtk-4.0'], header: 'spongy-gtk.h',
      install: true)
  endif
endif
