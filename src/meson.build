# Source files
sources = files(
  # Add your source files here
  # 'main.cpp',
)

# Main library
if sources.length() > 0
  e2sar_utils_lib = library('e2sar_utils',
    sources,
    include_directories : inc_dir,
    dependencies : project_deps,
    install : true,
  )

  # Declare dependency for use in other subprojects
  e2sar_utils_dep = declare_dependency(
    link_with : e2sar_utils_lib,
    include_directories : inc_dir,
    dependencies : project_deps
  )
endif

# Boost program_options for command-line parsing
boost_po_dep = dependency('boost', modules: ['program_options'])

# ROOT reader executable
root_reader_exe = executable('root-reader',
  'root_reader.cpp',
  include_directories : inc_dir,
  dependencies : [boost_po_dep, root_dep, e2sar_dep],
  install : true,
)
