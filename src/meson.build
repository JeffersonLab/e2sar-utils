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

# Executable (if needed)
# executable('e2sar-utils',
#   'main.cpp',
#   include_directories : inc_dir,
#   dependencies : project_deps,
#   install : true,
# )
