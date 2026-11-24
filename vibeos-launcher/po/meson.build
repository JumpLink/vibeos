# the launcher's translations: po/LANG.po for each language in LINGUAS, brought up to date with the sources by
#   tools/i18n/update-po.py vibeos-launcher
# (meson's vibeos-launcher-update-po would guess changed strings from the old ones). The test checks them.
import('i18n').gettext('vibeos-launcher', preset: 'glib', args: ['--add-location=file'])
test('po', find_program('python3'), args: ['-I', meson.project_source_root() / 'tools/i18n/check-po.py',
                                           meson.current_source_dir() / '..'],
     timeout: 300)  # it takes minutes in an emulated (qemu) arm64 build
