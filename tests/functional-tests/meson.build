test_runner = find_program('test-runner.sh')

config_json = configure_file(
  input: 'configuration.json.in',
  output: 'configuration.json',
  configuration: conf
)

functional_tests = [
  '300-miner-basic-ops',
  '301-miner-resource-removal',
  '310-fts-indexing',
  '400-extractor-metadata',
  '410-extractor-decorator',
  '500-writeback',
  '501-writeback-details',
  '600-applications-camera',
  '601-applications-sync',
]

config_json_full_path = join_paths(meson.current_build_dir(), 'configuration.json')
dconf_profile_full_path = join_paths(meson.current_source_dir(), 'trackertest')
tracker_extractors_dir = join_paths(meson.current_build_dir(), '..', '..', 'src', 'tracker-extract')
tracker_extractor_rules_dir = join_paths(meson.current_source_dir(), '..', '..', 'src', 'tracker-extract')

test_env = environment()
test_env.set('DCONF_PROFILE', dconf_profile_full_path)

test_env.set('TRACKER_EXTRACTORS_DIR', tracker_extractors_dir)
test_env.set('TRACKER_EXTRACTOR_RULES_DIR', tracker_extractor_rules_dir)
test_env.set('TRACKER_FUNCTIONAL_TEST_CONFIG', config_json_full_path)

foreach t: functional_tests
  test('functional-' + t, test_runner,
    args: './' + t + '.py',
    env: test_env,
    workdir: meson.current_source_dir(),
    # FIXME: these tests are all too slow
    timeout: 180)
endforeach
