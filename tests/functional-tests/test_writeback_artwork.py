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
import configuration
import fixtures
import gi
import os
import pathlib
import shutil

gi.require_version('Gst', '1.0')
from gi.repository import Gst

class WritebackAlbumArtTest(fixtures.TrackerWritebackTest):
    def _albumart_test(self, path, albumart_path):
        path = self.prepare_test_audio(self.datadir_path(path))
        initial_mtime = path.stat().st_mtime

        data = {"nmm:artwork": {"nie:isStoredAs": { "nie:url": pathlib.Path(albumart_path).as_uri()}}}
        resource = self.create_resource("nfo:Audio", path, data)
        self.writeback_data(resource.serialize())

        self.wait_for_file_change(path, initial_mtime)

        Gst.init(None)
        pipeline = Gst.parse_launch(f"filesrc location={path} ! decodebin ! fakesink")
        pipeline.set_state(Gst.State.PLAYING)
        bus = pipeline.get_bus()
        sample = None

        while True:
            message = bus.timed_pop_filtered(
                Gst.CLOCK_TIME_NONE, Gst.MessageType.TAG | Gst.MessageType.EOS)
            if message.type == Gst.MessageType.EOS:
                break
            elif message.type == Gst.MessageType.TAG:
                tag_list = message.parse_tag()
                success, sample = tag_list.get_sample(Gst.TAG_IMAGE)
                if success:
                    break

        pipeline.set_state(Gst.State.NULL)

        if not sample:
            raise Exception("No album art found after writeback")

        with open(albumart_path, "rb") as f:
            buf = sample.get_buffer()
            self.assertEqual(buf.memcmp(0, f.read()), 0)


    def test_mp3_artwork(self):
        albumart_path = os.path.join(
            os.path.dirname(__file__),
            "data/extractor-content/images/png-basic.png")

        self._albumart_test("writeback-test-5.mp3", albumart_path)

    def test_ogg_artwork(self):
        albumart_path = os.path.join(
            os.path.dirname(__file__),
            "data/extractor-content/images/png-basic.png")

        self._albumart_test("writeback-test-6.ogg", albumart_path)

    def test_flac_artwork(self):
        albumart_path = os.path.join(
            os.path.dirname(__file__),
            "data/extractor-content/images/png-basic.png")

        self._albumart_test("writeback-test-7.flac", albumart_path)

    def test_aac_artwork(self):
        albumart_path = os.path.join(
            os.path.dirname(__file__),
            "data/extractor-content/images/png-basic.png")

        self._albumart_test("writeback-test-8.mp4", albumart_path)

    def test_artwork_nonexistent(self):
        err = ""
        try:
            self._albumart_test("writeback-test-5.mp3", '/doesnotexist.png')
        except Exception as e:
            err = str(e)
        finally:
            self.assertIn("No album art", err);


if __name__ == "__main__":
    fixtures.tracker_test_main()
