commands_dir = get_option('bindir')
command_metadata_dir = get_option('datadir') / 'commands'
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

foreach c: commands
    command_name = 'tracker@0@-@1@'.format(tracker_api_major, c['name'])
    sources = c['sources']

    executable(command_name, sources,
        c_args: tracker_c_args + [
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

# The `tracker3` CLI loads all commands from one directory.
# We link the core tracke3 commands into our build tree so we can
# then set TRACKER_CLI_DIR to one location which has everything.
run_command('./link-core-cli-commands.sh', tracker_core_cli_commands_dir,
              check: true)
