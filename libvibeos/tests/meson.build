# vibeos.Theme, vibeos.Interface and the interface keys they go with, in a schema directory of the tests' own; the
# settings in memory only
test_schemas = custom_target('test-schemas',
  input: ['../../libtonal/data/vibeos.Theme.gschema.xml', 'org.gnome.desktop.interface.gschema.xml',
          '../data/vibeos.Interface.gschema.xml'],
  output: 'gschemas.compiled',
  command: [find_program('glib-compile-schemas'), '--strict', '--targetdir=@OUTDIR@',
            '--schema-file=@INPUT0@', '--schema-file=@INPUT1@', '--schema-file=@INPUT2@'])
test_theme = executable('test-theme', 'test-theme.c', test_schemas, dependencies: vibeos_dep)
# no system or user schemas at all (XDG_DATA_DIRS, XDG_DATA_HOME), so that what is installed on the build machine plays no part
test_env = ['GSETTINGS_BACKEND=memory', 'XDG_DATA_DIRS=/nonexistent', 'XDG_DATA_HOME=/nonexistent',
            'VIBEOS_COLOR_SCHEME=', 'VIBEOS_SEED=',
            'VIBEOS_CONTRAST=']
test('theme', test_theme, args: ['settings'],
  env: test_env + ['GSETTINGS_SCHEMA_DIR=' + meson.current_build_dir()])
test('theme-no-schema', test_theme, args: ['no-schema'], env: test_env + ['GSETTINGS_SCHEMA_DIR='])
test('theme-environment', test_theme, args: ['environment'],
  env: test_env + ['GSETTINGS_SCHEMA_DIR=' + meson.current_build_dir(), 'VIBEOS_COLOR_SCHEME=light',
                   'VIBEOS_SEED=#24222D'])

# Font size and Bold text: the curve and the stylesheets, then the settings as they change, the environment over them,
# and no schemas at all
test_text = executable('test-text', 'test-text.c', test_schemas, dependencies: vibeos_dep)
text_env = test_env + ['VIBEOS_FONT_SCALE=', 'VIBEOS_BOLD_TEXT=']
test('text', test_text, env: text_env + ['GSETTINGS_SCHEMA_DIR='])
test('text-settings', test_text, args: ['settings'], env: text_env + ['GSETTINGS_SCHEMA_DIR=' + meson.current_build_dir()])
test('text-environment', test_text, args: ['environment'],
  env: test_env + ['GSETTINGS_SCHEMA_DIR=' + meson.current_build_dir(), 'VIBEOS_FONT_SCALE=1.5', 'VIBEOS_BOLD_TEXT=0'])
test('text-no-schema', test_text, args: ['no-schema'], env: text_env + ['GSETTINGS_SCHEMA_DIR='])
