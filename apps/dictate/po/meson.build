# Dictate's and Vibe's translations (one domain, vibeos-dictate, for both apps): po/LANG.po for each language in
# LINGUAS, brought up to date with the sources by
#   tools/i18n/update-po.py apps/dictate
# Vibe's spoken lines are read from the same catalogues by dictate-l10n.c, by their msgid (and context, or plural
# pair) in the language Vibe speaks. (meson's vibeos-dictate-update-po would guess changed strings from the old
# ones.) The test checks them.
i18n.gettext('vibeos-dictate', preset: 'glib', args: ['--add-location=file'])
test('po', find_program('python3'), args: ['-I', meson.project_source_root() / '../../tools/i18n/check-po.py',
                                           meson.project_source_root()],
     timeout: 300)  # it takes minutes in an emulated (qemu) arm64 build
