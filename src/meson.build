e2sar_utils_lib = library('e2sar_utils',
  'event_data.cpp',
  'file_processor.cpp',
  include_directories : inc_dir,
  dependencies : project_deps,
  install : true,
)

e2sar_utils_dep = declare_dependency(
  link_with : e2sar_utils_lib,
  include_directories : inc_dir,
  dependencies : project_deps
)
