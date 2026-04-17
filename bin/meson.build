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
  link_with : e2sar_utils_lib,
  install : true,
)
