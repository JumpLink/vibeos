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

# the theme's colours by the original rules, against what the reference phone draws, in both modes
test_original = executable('test-original-colors', 'test-original-colors.c', test_schemas, dependencies: vibeos_dep)
test('original-colors', test_original, env: test_env + ['GSETTINGS_SCHEMA_DIR=' + meson.current_build_dir()])

# the locale from the session's environment, and dates and times in its language and order
test_locale = executable('test-locale', 'test-locale.c', test_schemas, dependencies: vibeos_dep)
test('locale', test_locale, env: test_env + ['GSETTINGS_SCHEMA_DIR=' + meson.current_build_dir()])

# a program starting over in the session's new language (vibeos-restart.c): in place, or by systemd for a Type=dbus
# service, on a bus of the test's own with a mock systemd user manager
test_restart_child = executable('test-restart-child', 'test-restart-child.c', '../vibeos-restart.c',
  include_directories: include_directories('..'), dependencies: dependency('gio-2.0'))
test('restart', find_program('python3'), args: [files('test-restart.py'), meson.current_build_dir()],
  depends: test_restart_child, env: test_env, timeout: 120)

# VibeosFitLabel's scale, measured in the font in libvibeos/fonts
test_fit = executable('test-fit', 'test-fit.c',
  dependencies: [vibeos_dep, dependency('fontconfig'), dependency('pangocairo')],
  c_args: ['-DFONT="@0@"'.format(meson.current_source_dir() / '..' / 'fonts' / 'VibeosRoboto.ttf')])
test('fit', test_fit, env: test_env)
