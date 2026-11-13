project('neo-calculator', 'c', version: '1.0', meson_version: '>= 1.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])
add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-sign-compare',
                      '-DGETTEXT_PACKAGE="neo-calculator"',
                      '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')), language: 'c')

gio = dependency('gio-2.0', version: '>= 2.76')
m = meson.get_compiler('c').find_library('m')
engine_deps = [gio, m, dependency('gmp'), dependency('sqlite3')]

# the arithmetic and the evaluator (ExactCalculator's CR, BoundedRational, UnifiedReal, CalculatorExpr,
# Evaluator, ExpressionDB): no GTK, shared by the app and the tests
engine = static_library('engine', 'src/calc-failure.c', 'src/creal.c', 'src/bounded-rational.c',
                        'src/unified-real.c', 'src/calc-keys.c', 'src/calc-expr.c', 'src/calc-format.c',
                        'src/expression-db.c', 'src/evaluator.c', dependencies: engine_deps)

# the app
gnome = import('gnome')
gtk = dependency('gtk4', version: '>= 4.14')
bindir = get_option('prefix') / get_option('bindir')
datadir = get_option('prefix') / get_option('datadir')
resources = gnome.compile_resources('calculator-resources', 'src/calculator.gresource.xml',
                                    source_dir: ['src', 'data'])
executable('neo-calculator', 'src/calc-main.c', 'src/calc-window.c', 'src/calc-display.c', 'src/calc-formula.c',
           'src/calc-result.c', 'src/calc-history.c', 'src/calc-pad.c', 'src/calc-button.c', 'src/calc-text.c',
           'src/calc-popup-menu.c', 'src/calc-text-toolbar.c', 'src/calc-toast.c', 'src/calc-licenses.c', resources,
           link_with: engine, install: true,
           dependencies: engine_deps + [gtk, dependency('neo-1'), dependency('spongy-gtk-1')])

configure_file(input: 'data/neo.Calculator.service.in', output: 'neo.Calculator.service',
               configuration: configuration_data({'bindir': bindir}), install_dir: datadir / 'dbus-1/services')
install_data('data/neo.Calculator.desktop', install_dir: datadir / 'applications')
install_data('data/neo.Calculator.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
install_data('data/neo.Calculator.gschema.xml', install_dir: datadir / 'glib-2.0/schemas')
gnome.post_install(glib_compile_schemas: true, gtk_update_icon_cache: true, update_desktop_database: true)

foreach name : ['creal', 'engine', 'evaluator']
  test(name, executable('test-' + name, 'tests/test-' + name + '.c', include_directories: 'src',
                        link_with: engine, dependencies: engine_deps),
       timeout: 300)
endforeach
