commands_dir = get_option('bindir')
command_metadata_dir = get_option('prefix') / get_option('datadir') / 'localsearch3' / 'commands'
po_dir = join_paths(meson.project_source_root(), 'po')

commands = [
  { 'name': 'daemon', 'sources': ['tracker-daemon.c', 'tracker-miner-manager.c', 'tracker-process.c'], 'extra_deps': [] },
  { 'name': 'extract', 'sources': ['tracker-extract.c', 'tracker-cli-utils.c'], 'extra_deps': [] },
  { 'name': 'index', 'sources': 'tracker-index.c', 'extra_deps': [] },
  { 'name': 'info', 'sources': ['tracker-info.c','tracker-cli-utils.c'], 'extra_deps': [] },
  { 'name': 'reset', 'sources': ['tracker-reset.c', 'tracker-miner-manager.c', 'tracker-process.c'], 'extra_deps': [] },
  { 'name': 'search', 'sources': 'tracker-search.c', 'extra_deps': [] },
  { 'name': 'status', 'sources': ['tracker-status.c', 'tracker-cli-utils.c', 'tracker-miner-manager.c'], 'extra_deps': [] },
  { 'name': 'tag', 'sources': 'tracker-tag.c', 'extra_deps': [] },
]

python_commands = [
  { 'name': 'test-sandbox', 'source': 'tracker-sandbox.in' },
]

main_command_name = 'localsearch@0@'.format(tracker_api_major)

executable(main_command_name,
    'main.c',
    c_args: tracker_c_args + [
        '-DCLI_METADATA_DIR="@0@"'.format(command_metadata_dir),
        '-DMANDIR="@0@"'.format(join_paths(get_option('prefix'), get_option('datadir'), 'man')),
        '-DBINDIR="@0@"'.format(join_paths(get_option('prefix'), get_option('bindir'))),
    ],
    install: true,
    install_dir: get_option('bindir'),
    dependencies: [tracker_sparql, tracker_miners_common_dep],
    include_directories: [commoninc, configinc, srcinc],
)

foreach c: commands
    command_name = 'localsearch@0@-@1@'.format(tracker_api_major, c['name'])
    sources = c['sources']

    executable(command_name, sources,
        c_args: tracker_c_args + [
            '-DMAIN_COMMAND_NAME=@0@'.format(main_command_name),
            '-DLIBEXECDIR="@0@"'.format(join_paths(get_option('prefix'), get_option('libexecdir'))),
            '-DBUILDROOT="@0@"'.format(build_root),
        ],
        install: true,
        install_dir: get_option('bindir'),
        dependencies: [tracker_sparql, tracker_miners_common_dep] + c['extra_deps'],
        include_directories: [commoninc, configinc, srcinc],
    )

    command_desktop_file = 'tracker-@0@.desktop'.format(c['name'])
    command_desktop = i18n.merge_file(
        input: command_desktop_file + '.in',
        output: command_desktop_file,
        install: true,
        install_dir: command_metadata_dir,
        po_dir: po_dir,
        type: 'desktop'
    )
endforeach

script_conf = configuration_data()
script_conf.set('typelibdir', typelib_dir)
script_conf.set('prefix', get_option('prefix'))
script_conf.set('testutils_dir', testutils_dir)

foreach c: python_commands
    command_name = 'localsearch@0@-@1@'.format(tracker_api_major, c['name'])

    configure_file(
      input: c['source'],
      output: command_name,
      configuration: script_conf,
      install_dir: get_option('prefix') / get_option('bindir'))

    command_desktop_file = 'tracker-@0@.desktop'.format(c['name'])
    command_desktop = i18n.merge_file(
        input: command_desktop_file + '.in',
        output: command_desktop_file,
        install: true,
        install_dir: command_metadata_dir,
        po_dir: po_dir,
        type: 'desktop'
    )
endforeach
