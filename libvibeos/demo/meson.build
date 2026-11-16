# vibeos-gallery: every layout widget on one page each, at any size (see gallery.c)
gallery_resources = gnome.compile_resources('gallery-resources', 'gallery.gresource.xml', c_name: 'gallery')
executable('vibeos-gallery', 'gallery.c', gallery_resources,
  dependencies: [vibeos_dep, libm],
  install: false)
