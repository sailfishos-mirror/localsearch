subdir('common')

subdir('libtracker-miners-common')

if have_tracker_extract
  subdir('libtracker-extract')
endif

# The test case for writeback doesn't seem to work.
#if enable_writeback
#  subdir('tracker-writeback')
#endif

subdir('services')

test_bus_conf_file = configure_file(
  input: 'test-bus.conf.in',
  output: 'test-bus.conf',
  configuration: conf)

if get_option('functional_tests')
  subdir('functional-tests')
endif
