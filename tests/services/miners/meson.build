# This directory contains service files used by tracker-miner-manager
# to identify which miners are available. These files are used
# when running Tracker from the build tree.

tracker_miner_fs_test_dbus_service_file = configure_file(
    input: '../org.freedesktop.Tracker3.Miner.Files.service.in',
    output: 'org.freedesktop.Tracker3.Miner.Files.service',
    configuration: conf)

tracker_uninstalled_miner_services_dir = meson.current_build_dir()
