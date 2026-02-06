# Tests
if get_option('enable_tests')
  # Add test framework dependency (e.g., Google Test, Catch2, etc.)
  # test_dep = dependency('gtest', main : true, required : false)

  # Test sources
  test_sources = files(
    # Add your test files here
    # 'test_main.cpp',
  )

  # Build and register tests
  # if test_dep.found() and test_sources.length() > 0
  #   test_exe = executable('e2sar_utils_tests',
  #     test_sources,
  #     dependencies : [project_deps, test_dep, e2sar_utils_dep],
  #   )
  #
  #   test('unit_tests', test_exe)
  # endif
endif

# E2SAR integration test (temporary)
# Needs Boost header paths since e2sar headers include Boost
test_e2sar_exe = executable('test-e2sar',
  'test_e2sar.cpp',
  dependencies : [
    e2sar_dep,
    boost_log_dep,
    boost_url_dep,
    boost_thread_dep,
    boost_chrono_dep,
    boost_filesystem_dep
  ],
  install : false,
)
