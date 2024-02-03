#!/bin/sh

set -e

core_cli_dir=$1

cli_dir=$MESON_BUILD_ROOT/$MESON_SUBDIR

for l in `find $cli_dir -type l`
do
    rm $l
done

# FIXME: it would be nice to not hardcode this list.
core_commands="endpoint export import sparql sql"

# Link to commands from tracker.git.
ln -s $core_cli_dir/tracker3 $cli_dir/tracker3
for c in $core_commands; do
    ln -s $core_cli_dir/tracker3-$c $cli_dir/tracker3-$c
    ln -s $core_cli_dir/tracker-$c.desktop $cli_dir/tracker-$c.desktop
done
