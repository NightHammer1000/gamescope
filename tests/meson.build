catch2_dep = dependency('catch2-with-main', required: true)

srcdir = '../src'
unittest_src = []
foreach src : gamescope_core_src + ['Script/Script.cpp', 'convar_script.cpp', 'Backends/DRMVendorQuirks.cpp', 'Backends/ScanoutModifiers.cpp']
	unittest_src += srcdir / src
endforeach

gamescope_tests = executable(
	'gamescope_tests',
	unittest_src + ['test_convar.cpp', 'test_process.cpp', 'test_scanout.cpp'],
	include_directories: [srcdir, sol2_include],
	dependencies: [catch2_dep, luajit_dep, cap_dep, drm_dep, glm_dep, wlroots_dep],
	cpp_args: gamescope_cpp_args,
)
test('convar', gamescope_tests, args: ['[convar]'])
test('process', gamescope_tests, args: ['[process]'])
test('scanout', gamescope_tests, args: ['[scanout]'])

# Plain assert-based test, not Catch2.
framegen_config_test = executable(
	'test_frame_generation_config',
	['test_frame_generation_config.cpp', srcdir / 'frame_generation_config.cpp'],
	include_directories: [srcdir],
	dependencies: [wlroots_dep],
)
test('frame_generation_config', framegen_config_test)
