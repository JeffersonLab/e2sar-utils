project('e2sar-utils', 'cpp',
  version : '0.1.0',
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
threads_dep = dependency('threads')

# Runtime dependencies
grpc_dep = dependency('grpc++', version : '>=1.74.1', required : true)
protobuf_dep = dependency('protobuf', required : true)

# Collect all dependencies
project_deps = [
  boost_dep,
  threads_dep,
  grpc_dep,
  protobuf_dep
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
