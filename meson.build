project('e2sar-utils', 'cpp',
  version : files('VERSION.txt'),
  default_options : [
    'warning_level=3',
    'cpp_std=c++17',
    'buildtype=debugoptimized'
  ]
)

# C++ compiler
cxx = meson.get_compiler('cpp')

# Compile-time dependencies
boost_dep = dependency('boost', version : '>=1.89.0')
boost_url_dep = dependency('boost', modules: ['url'], version : '>=1.89.0')
boost_log_dep = dependency('boost', modules: ['log'], version : '>=1.89.0')
boost_thread_dep = dependency('boost', modules: ['thread'], version : '>=1.89.0')
boost_chrono_dep = dependency('boost', modules: ['chrono'], version : '>=1.89.0')
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
# Get e2sar from pkg-config, but manually add its missing dependencies
# The e2sar.pc file doesn't declare all transitive dependencies needed for static linking
e2sar_base = dependency('e2sar', method: 'pkg-config', required: true)

e2sar_dep = declare_dependency(
  dependencies: [
    e2sar_base,
    boost_url_dep,
    boost_log_dep,
    boost_thread_dep,
    boost_chrono_dep,
    grpc_dep,
    protobuf_dep
  ]
)

# Collect all dependencies
project_deps = [
  boost_dep,
  threads_dep,
  grpc_dep,
  protobuf_dep,
  root_dep,
  e2sar_dep
]

# Include directories
inc_dir = include_directories('include')

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
