dep_xdamage = dependency('xdamage')
dep_xcomposite = dependency('xcomposite')
dep_xcursor = dependency('xcursor')
dep_xrender = dependency('xrender')
dep_xext = dependency('xext')
dep_xfixes = dependency('xfixes')
dep_xxf86vm = dependency('xxf86vm')
dep_xtst = dependency('xtst')
dep_xres = dependency('xres')
dep_xmu = dependency('xmu')
dep_xi = dependency('xi')

# Telescope is a DRM session compositor: a build without it is not this project.
# The HAVE_DRM guards stay, deliberately -- rewriting them would cost upstream merges.
drm_dep = dependency('libdrm', version: '>= 2.4.113', required: true)
gbm_dep = dependency('gbm', version: '>= 21.3', required: true)
eis_dep = dependency('libeis-1.0', required : get_option('input_emulation'))

wayland_server = dependency('wayland-server', version: '>=1.21')
wayland_protos = dependency('wayland-protocols', version: '>=1.17')
xkbcommon = dependency('xkbcommon')
thread_dep = dependency('threads')
cap_dep = dependency('libcap', required: get_option('rt_cap'))
epoll_dep = dependency('epoll-shim', required: false)
avif_dep = dependency('libavif', version: '>=1.0.0', required: get_option('avif_screenshots'))
pixman_dep = dependency('pixman-1')
udev_dep = dependency('libudev')

wlroots_dep = dependency(
  'wlroots-0.20',
  version: ['>= 0.20.0', '< 0.21.0'],
  fallback: 'wlroots',
  default_options: ['default_library=static', 'examples=false', 'xwayland=enabled', 'backends=libinput', 'renderers=[]', 'allocators=[]', 'session=enabled'],
)

displayinfo_dep = dependency(
  'libdisplay-info',
  version: ['>= 0.0.0', '< 0.4.0'],
  fallback: 'libdisplay-info',
  default_options: ['default_library=static'],
)

libdecor_dep = dependency('libdecor-0')

glsl_compiler = find_program('glslang', 'glslangValidator', native: true)

# Use --depfile to rebuild shaders when included files have changed. Sadly debian based
# distros don't have up-to-date glslang so we need to check for support first.
if run_command(glsl_compiler, ['--version', '--depfile', 'dummy.dep'], check: false).returncode() == 0
  glsl_generator = generator(
    glsl_compiler,
    output    : ['@BASENAME@.h'],
    arguments : ['-V', '@INPUT@', '--vn', '@BASENAME@', '-o', '@OUTPUT@', '--depfile', '@DEPFILE@', '--quiet'],
    depfile   : '@BASENAME@.h.d',
  )
else
  glsl_generator = generator(
    glsl_compiler,
    output    : ['@BASENAME@.h'],
    arguments : ['-V', '@INPUT@', '--vn', '@BASENAME@', '-o', '@OUTPUT@'],
  )
endif

shader_src = [
  'shaders/cs_composite_blit.comp',
  'shaders/cs_composite_blit_rgb10a2.comp',
  'shaders/cs_composite_blur.comp',
  'shaders/cs_composite_blur_cond.comp',
  'shaders/cs_composite_rcas.comp',
  'shaders/cs_composite_rcas_fp16.comp',
  'shaders/cs_composite_rcas_rgba16f.comp',
  'shaders/cs_composite_rcas_rgb10a2.comp',
  'shaders/cs_easu.comp',
  'shaders/cs_easu_fp16.comp',
  'shaders/cs_easu_rgba16f.comp',
  'shaders/cs_easu_fp16_rgba16f.comp',
  'shaders/cs_gaussian_blur_horizontal.comp',
  'shaders/cs_nis.comp',
  'shaders/cs_nis_fp16.comp',
  'shaders/cs_rgb_to_nv12.comp',
]

spirv_shaders = glsl_generator.process(shader_src)

# FidelityFX SDK 1.1.4 is the last upstream SDK release with maintained
# Vulkan/GLSL frame-generation shaders. Keep these as standalone shader
# modules; Gamescope owns resource allocation and scheduling.
ffx_gpu_path = meson.current_source_dir() / '../subprojects/FidelityFX-SDK/sdk/include/FidelityFX/gpu'
ffx_shader_path = meson.current_source_dir() / '../subprojects/FidelityFX-SDK/sdk/src/backends/vk/shaders/opticalflow'
ffx_fi_shader_path = meson.current_source_dir() / '../subprojects/FidelityFX-SDK/sdk/src/backends/vk/shaders/frameinterpolation'
ffx_opticalflow_shader_names = [
  'ffx_opticalflow_prepare_luma_pass',
  'ffx_opticalflow_compute_luminance_pyramid_pass',
  'ffx_opticalflow_generate_scd_histogram_pass',
  'ffx_opticalflow_compute_scd_divergence_pass',
  'ffx_opticalflow_compute_optical_flow_advanced_pass_v5',
  'ffx_opticalflow_filter_optical_flow_pass_v5',
  'ffx_opticalflow_scale_optical_flow_advanced_pass_v5',
]

ffx_spirv_shaders = []
foreach shader_name : ffx_opticalflow_shader_names
  ffx_spirv_shaders += custom_target(
    shader_name + '.h',
    output: shader_name + '.h',
    command: [
      glsl_compiler,
      '-V', '--target-env', 'vulkan1.1', '-S', 'comp',
      '-DFFX_GPU=1', '-DFFX_GLSL=1', '-I' + ffx_gpu_path,
      ffx_shader_path / (shader_name + '.glsl'),
      '--vn', shader_name, '-o', '@OUTPUT@',
      '--depfile', '@DEPFILE@', '--quiet',
    ],
    depfile: shader_name + '.h.d',
  )
endforeach

ffx_frameinterpolation_shader_names = [
  'ffx_frameinterpolation_optical_flow_vector_field_pass',
  'ffx_frameinterpolation_compute_inpainting_pyramid_pass',
  'ffx_frameinterpolation_inpainting_pass',
]

foreach shader_name : ffx_frameinterpolation_shader_names
  ffx_spirv_shaders += custom_target(
    shader_name + '.h',
    output: shader_name + '.h',
    command: [
      glsl_compiler,
      '-V', '--target-env', 'vulkan1.1', '-S', 'comp',
      '-DFFX_GPU=1', '-DFFX_GLSL=1', '-I' + ffx_gpu_path,
      ffx_fi_shader_path / (shader_name + '.glsl'),
      '--vn', shader_name, '-o', '@OUTPUT@',
      '--depfile', '@DEPFILE@', '--quiet',
    ],
    depfile: shader_name + '.h.d',
  )
endforeach

ffx_midpoint_shader_name = 'cs_ffx_frameinterpolation_midpoint'
ffx_spirv_shaders += custom_target(
  ffx_midpoint_shader_name + '.h',
  output: ffx_midpoint_shader_name + '.h',
  command: [
    glsl_compiler,
    '-V', '--target-env', 'vulkan1.1', '-S', 'comp',
    '-DFFX_GPU=1', '-DFFX_GLSL=1', '-I' + ffx_gpu_path,
    meson.current_source_dir() / 'shaders' / (ffx_midpoint_shader_name + '.comp'),
    '--vn', ffx_midpoint_shader_name, '-o', '@OUTPUT@',
    '--depfile', '@DEPFILE@', '--quiet',
  ],
  depfile: ffx_midpoint_shader_name + '.h.d',
)

ffx_gui_mask_shader_name = 'cs_ffx_frameinterpolation_gui_mask'
ffx_spirv_shaders += custom_target(
  ffx_gui_mask_shader_name + '.h',
  output: ffx_gui_mask_shader_name + '.h',
  command: [
    glsl_compiler,
    '-V', '--target-env', 'vulkan1.1', '-S', 'comp',
    '-DFFX_GPU=1', '-DFFX_GLSL=1', '-I' + ffx_gpu_path,
    meson.current_source_dir() / 'shaders' / (ffx_gui_mask_shader_name + '.comp'),
    '--vn', ffx_gui_mask_shader_name, '-o', '@OUTPUT@',
    '--depfile', '@DEPFILE@', '--quiet',
  ],
  depfile: ffx_gui_mask_shader_name + '.h.d',
)

ffx_scaled_luma_shader_name = 'cs_ffx_opticalflow_prepare_luma_scaled'
ffx_spirv_shaders += custom_target(
  ffx_scaled_luma_shader_name + '.h',
  output: ffx_scaled_luma_shader_name + '.h',
  command: [
    glsl_compiler,
    '-V', '--target-env', 'vulkan1.1', '-S', 'comp',
    '-DFFX_GPU=1', '-DFFX_GLSL=1', '-I' + ffx_gpu_path,
    meson.current_source_dir() / 'shaders' / (ffx_scaled_luma_shader_name + '.comp'),
    '--vn', ffx_scaled_luma_shader_name, '-o', '@OUTPUT@',
    '--depfile', '@DEPFILE@', '--quiet',
  ],
  depfile: ffx_scaled_luma_shader_name + '.h.d',
)

# The dot-product SAD search needs GL_EXT_integer_dot_product, which older
# glslang builds (Ubuntu 24.04) do not know. Probe, and fall back to the
# regular search pass at build time; runtime already gates on device support.
ffx_have_dot_search = run_command(
  'sh', '-c',
  'printf "#version 460\n#extension GL_EXT_integer_dot_product : require\nvoid main(){}\n" | ' +
  glsl_compiler.full_path() + ' --stdin -S comp --target-env vulkan1.3 -V -o /dev/null',
  check: false,
).returncode() == 0

ffx_dot_search_shader_name = 'cs_ffx_opticalflow_compute_optical_flow_dot'
if ffx_have_dot_search
  ffx_spirv_shaders += custom_target(
    ffx_dot_search_shader_name + '.h',
    output: ffx_dot_search_shader_name + '.h',
    command: [
      glsl_compiler,
      '-V', '--target-env', 'vulkan1.1', '-S', 'comp',
      '-DFFX_GPU=1', '-DFFX_GLSL=1', '-I' + ffx_gpu_path,
      meson.current_source_dir() / 'shaders' / (ffx_dot_search_shader_name + '.comp'),
      '--vn', ffx_dot_search_shader_name, '-o', '@OUTPUT@',
      '--depfile', '@DEPFILE@', '--quiet',
    ],
    depfile: ffx_dot_search_shader_name + '.h.d',
  )
endif

ffx_vector_field_shader_name = 'cs_ffx_frameinterpolation_vector_field'
ffx_spirv_shaders += custom_target(
  ffx_vector_field_shader_name + '.h',
  output: ffx_vector_field_shader_name + '.h',
  command: [
    glsl_compiler,
    '-V', '--target-env', 'vulkan1.1', '-S', 'comp',
    '-DFFX_GPU=1', '-DFFX_GLSL=1', '-I' + ffx_gpu_path,
    meson.current_source_dir() / 'shaders' / (ffx_vector_field_shader_name + '.comp'),
    '--vn', ffx_vector_field_shader_name, '-o', '@OUTPUT@',
    '--depfile', '@DEPFILE@', '--quiet',
  ],
  depfile: ffx_vector_field_shader_name + '.h.d',
)

sol2_include = include_directories(['../thirdparty'])

required_wlroots_features = ['xwayland']

src = [
  'Backends/HeadlessBackend.cpp',
  'Utils/DirHelpers.cpp',
  'Utils/TempFiles.cpp',
  'Utils/Version.cpp',
  'Utils/Process.cpp',
  'Script/Script.cpp',
  'BufferMemo.cpp',
  'DmabufSync.cpp',
  'steamcompmgr.cpp',
  'convar.cpp',
  'convar_script.cpp',
  'commit.cpp',
  'frame_generation_config.cpp',
  'frame_generation_vulkan.cpp',
  'color_helpers.cpp',
  'main.cpp',
  'edid.cpp',
  'wlserver.cpp',
  'vblankmanager.cpp',
  'rendervulkan.cpp',
  'log.cpp',
  'ime.cpp',
  'mangoapp.cpp',
  'Timeline.cpp',
  'backend.cpp',
  'x11cursor.cpp',
  'InputEmulation.cpp',
  'LibInputHandler.cpp',
]

luajit_dep = dependency( 'luajit' )
libinput_dep = dependency('libinput', required: true)

gamescope_cpp_args = []
if drm_dep.found()
  src += 'Backends/DRMBackend.cpp'
  src += 'Backends/DRMGbmScanout.cpp'
  src += 'Backends/DRMVendorQuirks.cpp'
  src += 'Backends/ScanoutModifiers.cpp'
  src += 'modegen.cpp'
  required_wlroots_features += 'libinput_backend'
  liftoff_dep = dependency(
    'libliftoff',
    version: ['>= 0.5.0', '< 0.6.0'],
    fallback: 'libliftoff',
    default_options: ['default_library=static'],
  )
else
  liftoff_dep = dependency('', required: false)
endif

gamescope_cpp_args += '-DHAVE_DRM=@0@'.format(drm_dep.found().to_int())
gamescope_cpp_args += '-DHAVE_GBM=@0@'.format((drm_dep.found() and gbm_dep.found()).to_int())
gamescope_cpp_args += '-DHAVE_AVIF=@0@'.format(avif_dep.found().to_int())
gamescope_cpp_args += '-DHAVE_LIBCAP=@0@'.format(cap_dep.found().to_int())
gamescope_cpp_args += '-DHAVE_LIBEIS=@0@'.format(eis_dep.found().to_int())
gamescope_cpp_args += '-DHAVE_SCRIPTING=1'
gamescope_cpp_args += '-DHAVE_FFX_DOT_SEARCH=@0@'.format(ffx_have_dot_search.to_int())

src += spirv_shaders
src += ffx_spirv_shaders
src += protocols_server_src
src += protocols_client_src

if pipewire_dep.found()
  src += 'pipewire.cpp'
endif

foreach feat : required_wlroots_features
  if wlroots_dep.get_variable('have_' + feat) != 'true'
    error('Cannot use wlroots built without ' + feat + ' support')
  endif
endforeach

cc = meson.get_compiler('c')
compiler_name = cc.get_id()
compiler_version = cc.version()

vcs_tag_cmd = ['git', 'describe', '--always', '--tags', '--dirty=+']
vcs_tag = run_command(vcs_tag_cmd, check: false).stdout().strip()
version_tag = vcs_tag + ' (' + compiler_name + ' ' + compiler_version + ')'

gamescope_version_conf = configuration_data()
gamescope_version_conf.set('VCS_TAG', version_tag)

gamescope_version = configure_file(
  input : 'GamescopeVersion.h.in',
  output : 'GamescopeVersion.h',
  configuration : gamescope_version_conf
)

  executable(
    'telescope',
    src, gamescope_version,
    include_directories : [sol2_include],
    dependencies: [
      dep_wayland, dep_x11, dep_xdamage, dep_xcomposite, dep_xrender, dep_xext, dep_xfixes,
      dep_xxf86vm, dep_xres, glm_dep, drm_dep, gbm_dep, wayland_server,
      xkbcommon, thread_dep, wlroots_dep,
      vulkan_dep, liftoff_dep, dep_xtst, dep_xmu, cap_dep, epoll_dep, pipewire_dep, librt_dep,
      stb_dep, displayinfo_dep, dep_xcursor, avif_dep, dep_xi,
      eis_dep, luajit_dep, libinput_dep, pixman_dep, udev_dep,
    ],
    install: true,
    cpp_args: gamescope_cpp_args,
  )

gamescope_core_src = [
  'convar.cpp',
  'log.cpp',
  'Utils/DirHelpers.cpp',
  'Utils/Process.cpp',
  'Utils/Version.cpp',
]

if pipewire_dep.found()
  executable( 'telescopestream', ['Apps/telescopestream.cpp'], gamescope_core_src, gamescope_version, protocols_client_src, dependencies: [ pipewire_dep, dep_wayland, libdecor_dep, cap_dep ], install: true )
endif

executable('telescopereaper', ['Apps/telescopereaper.cpp', gamescope_core_src], gamescope_version, dependencies: [cap_dep], install:true )

benchmark_dep = dependency('benchmark', required: get_option('benchmark'), disabler: true)
executable('telescope_color_microbench', ['color_bench.cpp', 'color_helpers.cpp'], gamescope_core_src, gamescope_version, dependencies:[benchmark_dep, glm_dep, cap_dep])

executable('telescope_color_tests', ['color_tests.cpp', 'color_helpers.cpp'], gamescope_core_src, gamescope_version, dependencies:[glm_dep, cap_dep])

executable('telescopectl', ['Apps/telescopectl.cpp'], gamescope_core_src, gamescope_version, protocols_client_src, dependencies: [dep_wayland, cap_dep], install:true )

executable('telescope-type', ['Apps/telescope_type.c'], protocols_client_src, dependencies: [dep_wayland, cap_dep], install:true )

executable('telescope_hotkey_example', ['Apps/telescope_hotkey_example.cpp'], gamescope_core_src, gamescope_version, protocols_client_src, dependencies: [dep_wayland, xkbcommon, cap_dep], install: false )
