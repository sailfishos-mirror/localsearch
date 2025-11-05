# Copyright (C) 2025, Carlos Garnacho <carlosg@gnome.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
# Boston, MA  02110-1301, USA.


import unittest

import fixtures

import gi
from gi.repository import GLib


class WritebackAudioTest(fixtures.TrackerWritebackTest):
    def _writeback_test(self, path, data):
        path = self.prepare_test_audio(self.datadir_path(path))
        initial_mtime = path.stat().st_mtime

        resource = self.create_resource("nfo:MediaList", path, data)
        self.writeback_data(resource.serialize())

        self.wait_for_file_change(path, initial_mtime)
        self.check_data(path, data)

    def test_playlist_entries(self):
        self._writeback_test(
            "writeback-test-3.m3u",
            {"nfo:hasMediaFileListEntry": [{"nfo:listPosition": 1,
                                            "nfo:entryUrl": "http://example.com/a"},
                                           {"nfo:listPosition": 2,
                                            "nfo:entryUrl": "http://example.com/b"}]})

if __name__ == "__main__":
    fixtures.tracker_test_main()
