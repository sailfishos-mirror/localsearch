scraper_gresources = gnome.compile_resources(
    'scraper-resources',
    'org.freedesktop.Tracker3.Scraper.gresource.xml',
    c_name: 'tracker_scraper',
)

tracker_scraper_sources = files(
  'tracker-main.c',
  'tracker-scraper.c',
)

executable('tracker-scraper-@0@'.format(tracker_api_major),
  tracker_scraper_sources,
  scraper_gresources,

  # Manually add the root dir to work around https://github.com/mesonbuild/meson/issues/1387
  c_args: tracker_c_args + ['-I' + meson.build_root()],
  dependencies: [tracker_sparql, tracker_miners_common_dep],
  install: true,
  install_dir: join_paths(get_option('prefix'), get_option('libexecdir')),
  install_rpath: tracker_internal_libs_dir)
