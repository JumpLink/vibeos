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

foreach name : ['creal', 'engine', 'evaluator']
  test(name, executable('test-' + name, 'tests/test-' + name + '.c', include_directories: 'src',
                        link_with: engine, dependencies: engine_deps),
       timeout: 300)
endforeach
