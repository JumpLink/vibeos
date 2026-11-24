# The extension's translations: po/LANG.po for each language in LINGUAS, brought up to date with the sources by
#   tools/i18n/update-po.py extension
# (meson's vibeos-shell-update-po would guess changed strings from the old ones). The test checks them, where the
# source tree has the tools.
i18n.gettext('vibeos-shell', preset: 'glib', args: ['--add-location=file'])
check_po = meson.project_source_root() / '../tools/i18n/check-po.py'
if import('fs').exists(check_po)
  test('po', find_program('python3'), args: ['-I', check_po, meson.project_source_root()],
       timeout: 300)  # it takes minutes in an emulated (qemu) arm64 build
endif
