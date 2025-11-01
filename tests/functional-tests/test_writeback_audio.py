# Copyright (C) 2019-2020, Sam Thursfield (sam@afuera.me.uk)
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

        resource = self.create_resource("nfo:Audio", path, data)
        self.writeback_data(resource.serialize())

        self.wait_for_file_change(path, initial_mtime)
        self.check_data(path, data)

    def test_mp3_title(self):
        self._writeback_test(
            "writeback-test-5.mp3",
            {"nie:title": "test_title"})

    def test_ogg_title(self):
        self._writeback_test(
            "writeback-test-6.ogg",
            {"nie:title": "test_title"})

    def test_flac_title(self):
        self._writeback_test(
            "writeback-test-7.flac",
            {"nie:title": "test_title"})

    def test_aac_title(self):
        self._writeback_test(
            "writeback-test-8.mp4",
            {"nie:title": "test_title"})


if __name__ == "__main__":
    fixtures.tracker_test_main()
