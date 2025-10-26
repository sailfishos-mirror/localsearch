libtracker_extract_sources = [
  'tracker-encoding.c',
  'tracker-guarantee.c',
  'tracker-resource-helpers.c',
  'tracker-utils.c',
]

if charset_library_name == 'enca'
  libtracker_extract_sources += 'tracker-encoding-enca.c'
elif charset_library_name == 'icu'
  libtracker_extract_sources += 'tracker-encoding-libicu.c'
endif

tracker_extract_dependencies = [charset_library, gmodule, tracker_sparql]

if exempi.found()
  libtracker_extract_sources += ['tracker-xmp.c']
  tracker_extract_dependencies += exempi
endif

if gexiv2.found()
  libtracker_extract_sources += ['tracker-exif.c', 'tracker-iptc.c']
  tracker_extract_dependencies += gexiv2
endif

libtracker_extract = library('tracker-extract',
  libtracker_extract_sources,
  dependencies: [tracker_miners_common_dep] + tracker_extract_dependencies,
  c_args: tracker_c_args,
  install: true,
  install_dir: tracker_internal_libs_dir,
)

tracker_extract_dep = declare_dependency(
  link_with: libtracker_extract,
  dependencies: [tracker_extract_dependencies, tracker_miners_common_dep],
  include_directories: [srcinc, include_directories('.')]
)
