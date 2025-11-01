# Copyright (C) 2010, Nokia (ivan.frade@nokia.com)
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
#

"""Tests for Tracker writeback daemon."""


import logging
import unittest as ut

import configuration
import fixtures

import gi
from gi.repository import GLib

log = logging.getLogger(__name__)


class WritebackImagesTest(fixtures.TrackerWritebackTest):
    """
    Write in tracker store the properties witih writeback support and check
    that the new values are actually in the file
    """

    def __writeback_test(self, filename, data):
        """
        Set a value in @prop for the @filename. Then ask tracker-extractor
        for metadata and check in the results dictionary if the property is there.

        Note: given the special translation of some property-names in the dictionary
        with extracted metadata, there is an optional parameter @expectedKey
        to specify what property to check in the dictionary. If None, then
        the @prop is used.
        """

        path = self.prepare_test_image(self.datadir_path(filename))
        initial_mtime = path.stat().st_mtime

        resource = self.create_resource("nfo:Image", path, data)
        self.writeback_data(resource.serialize())

        log.debug("Waiting for change on %s", path)
        self.wait_for_file_change(path, initial_mtime)
        log.debug("Got the change")
        self.check_data(path, data)

    # JPEG test

    def test_001_jpeg_title(self):
        self.__writeback_test(
            "writeback-test-1.jpeg",
            {"nie:title": "test_title"})

    def test_002_jpeg_description(self):
        self.__writeback_test(
            "writeback-test-1.jpeg",
            {"nie:description": "test_description"})

    def test_003_jpeg_hasTag (self):
        self.__writeback_test (
            "writeback-test-1.jpeg",
            {"nao:hasTag" : {"nao:prefLabel": "test_tag"}})

    # TIFF tests

    def test_011_tiff_title(self):
        self.__writeback_test(
            "writeback-test-2.tif",
            {"nie:title": "test_title"})

    def test_012_tiff_description(self):
        self.__writeback_test(
            "writeback-test-2.tif",
            {"nie:description": "test_description"})

    def test_013_tiff_hasTag (self):
        self.__writeback_test (
            "writeback-test-2.tif",
            {"nao:hasTag": {"nao:prefLabel": "test_tag"}})

    # PNG tests

    def test_021_png_title(self):
        self.__writeback_test(
            "writeback-test-4.png",
            {"nie:title": "test_title"})

    def test_022_png_description(self):
        self.__writeback_test(
            "writeback-test-4.png",
            {"nie:description": "test_description"})

    def test_023_png_hasTag (self):
        self.__writeback_test (
            "writeback-test-4.png",
            {"nao:hasTag": {"nao:prefLabel": "test_tag"}})


if __name__ == "__main__":
    fixtures.tracker_test_main()
