project('e2sar-utils', 'cpp',
  version : files('VERSION.txt'),
  default_options : [
    'warning_level=3',
    'cpp_std=c++17',
    'buildtype=debugoptimized'
  ]
)

enable_et = get_option('enable_et')

# C++ compiler
cxx = meson.get_compiler('cpp')

# Compile-time dependencies
# Boost dependencies: program_options used by root-reader, others required by e2sar
# Note: Boost doesn't provide .pc files, so we must declare them explicitly even though
# e2sar.pc lists them in Requires (which pkg-config cannot resolve for Boost)
boost_program_options_dep = dependency('boost', modules: ['program_options'], version : '>=1.89.0')
boost_log_dep = dependency('boost', modules: ['log'], version : '>=1.89.0')
boost_url_dep = dependency('boost', modules: ['url'], version : '>=1.89.0')
boost_thread_dep = dependency('boost', modules: ['thread'], version : '>=1.89.0')
boost_chrono_dep = dependency('boost', modules: ['chrono'], version : '>=1.89.0')
boost_filesystem_dep = dependency('boost', modules: ['filesystem'], version : '>=1.89.0')
threads_dep = dependency('threads')

# Runtime dependencies
grpc_dep = dependency('grpc++', version : '>=1.74.1', required : true)
protobuf_dep = dependency('protobuf', required : true)

# ROOT dependency for particle physics data
root_dep = dependency('ROOT',
                      method: 'pkg-config',
                      required: false,
                      components: ['Core', 'RIO', 'Tree'])

if not root_dep.found()
  # Fallback to root-config
  root_config = find_program('root-config', required: true)
  root_cflags = run_command(root_config, '--cflags', check: true).stdout().strip()
  root_libs = run_command(root_config, '--libs', check: true).stdout().strip()

  root_dep = declare_dependency(
    compile_args: root_cflags.split(),
    link_args: root_libs.split()
  )
endif

# E2SAR dependency - EJFAT Event Segmentation And Reassembly
# The e2sar.pc now has Boost linker flags in Libs.private and grpc++/protobuf in Requires
# We still need to declare Boost modules explicitly for header paths (Boost has no .pc files)
e2sar_dep = dependency('e2sar', method: 'pkg-config', required: true)

# ET dependency (only when enable_et=true)
if enable_et
  coda_root = run_command('sh', '-c', 'printf %s "$CODA"', check: true).stdout().strip()
  if coda_root == ''
    error('enable_et=true but environment variable CODA is not set')
  endif

  et_incdir = coda_root + '/include'
  et_libdir = coda_root + '/Linux-86_64/lib'

  # Meson 0.61: external libraries must not go in link_with; pass via link_args.
  # ET include path passed as compile flag since it's an external dependency
  et_dep = declare_dependency(
    compile_args : ['-I' + et_incdir],
    link_args : ['-L' + et_libdir, '-let']
  )
endif

# Collect all dependencies
# Note: Boost modules declared for headers (no .pc files), linker flags come from e2sar.pc
project_deps = [
  boost_program_options_dep,
  boost_log_dep,
  boost_url_dep,
  boost_thread_dep,
  boost_chrono_dep,
  boost_filesystem_dep,
  threads_dep,
  root_dep,
  e2sar_dep
]

# Subdirectories
subdir('src')
subdir('tests')

# Summary
summary({
  'prefix': get_option('prefix'),
  'bindir': get_option('bindir'),
  'libdir': get_option('libdir'),
  'datadir': get_option('datadir'),
}, section: 'Directories')

summary({
  'Build type': get_option('buildtype'),
  'C++ standard': get_option('cpp_std'),
}, section: 'Configuration')
