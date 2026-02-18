# Shared common code
subdir('common')

# Filesystem indexer
if have_tracker_miner_fs
  subdir('indexer')
endif

# Public data extract tool & modules
if get_option('extract')
  subdir('extractor')
endif

# Control Proxy
subdir('control')

# Metadata writeback service & modules
if get_option('writeback')
  subdir('writeback')
endif

# CLI subcommands
subdir('cli')
