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

# ROOT reader executable
# Needs all Boost modules that e2sar depends on (for headers and linking)
e2sar_root_exe = executable('e2sar-root',
  'e2sar_root.cpp',
  include_directories : inc_dir,
  dependencies : [
    boost_program_options_dep,
    boost_log_dep,
    boost_url_dep,
    boost_thread_dep,
    boost_chrono_dep,
    boost_filesystem_dep,
    root_dep,
    e2sar_dep
  ],
  install : true,
)
