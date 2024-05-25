daemon_manpages = [
    ['localsearch-3', 1],
    ['localsearch-writeback-3', 1],
]

if have_tracker_miner_rss
  daemon_manpages += [['tracker-miner-rss-3', 1]]
endif

cli_manpages = [
  ['localsearch3-daemon', 1],
  ['localsearch3-extract', 1],
  ['localsearch3-index', 1],
  ['localsearch3-info', 1],
  ['localsearch3-reset', 1],
  ['localsearch3-search', 1],
  ['localsearch3-status', 1],
  ['localsearch3-tag', 1],
]

foreach m : daemon_manpages + cli_manpages
  name = m[0]
  section = m[1]

  manpage = '@0@.@1@'.format(name, section)

  xml = custom_target(manpage,
    command: [a2x,
              '--attribute=author=The LocalSearch developers',
              '--attribute=manversion=@0@'.format(meson.project_version()),
              '--attribute=manmanual=LocalSearch manual',
              '--doctype=manpage',
              '--format=manpage',
              '--no-xmllint',
              '--xsltproc-opts=' + ' '.join([
                '--stringparam', 'man.authors.section.enabled', '0',
              ]),
              '-D', '@OUTDIR@',
              '@INPUT@'],
    input: manpage + '.txt',
    output: manpage,
    install: true,
    install_dir: get_option('mandir') / 'man@0@'.format(section),
  )
endforeach
