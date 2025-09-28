# Copyright (C) 2021, Codethink Ltd
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
# Author: Sam Thursfield <sam@afuera.me.uk>


import logging
import pathlib

import fixtures
from fixtures import UnmountFlags

from gi.repository import GLib

import time

log = logging.getLogger(__name__)


class MinerRemovableMediaTest(fixtures.TrackerMinerRemovableMediaTest):
    """Tests for tracker-miner-fs with index-removable-devices feature."""

    def setUp(self):
        super(MinerRemovableMediaTest, self).setUp()

        self.device_path = pathlib.Path(self.workdir).joinpath("removable-device-1")
        self.device_path.mkdir()

    def ensure_document_in_removable(self, endpoint_helper, uri):
        return endpoint_helper.ensure_resource(
            fixtures.DOCUMENTS_GRAPH, f"nie:isStoredAs <{uri}>",
        )

    def data_source_available(self, endpoint_helper, uri):
        """Check that `uri` exists in the device database"""

        try:
            result = endpoint_helper.query(
                """
                SELECT ?u { ?u nie:url <%s> }
                """
                % uri
            )
            return True if len(result) > 0 and result[0][0] == uri else False
        except:
            return False

    def create_test_data(self):
        files = ["file1.txt", "dir1/file2.txt"]

        for f in files:
            path = self.device_path.joinpath(f)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("This file exists.")

        return files

    def test_add_remove_device(self):
        """Device should be indexed by Tracker when connected."""

        files = self.create_test_data()

        object_path = self.miner_fs.removable_device_object_path (self.device_path)
        self.add_removable_device(self.device_path)
        self.miner_fs.await_endpoint_added(self.device_path.as_uri())
        endpoint_helper = self.helper_for_endpoint(object_path)

        for f in files:
            rel_uri = self.get_relative_uri(f)
            self.ensure_document_in_removable(endpoint_helper, rel_uri)
            assert self.data_source_available(endpoint_helper, rel_uri)

        with self.miner_fs.await_endpoint_removed(self.device_path.as_uri()):
            self.remove_removable_device(self.device_path)

        for f in files:
            rel_uri = self.get_relative_uri(f)
            assert not self.data_source_available(endpoint_helper, rel_uri), (
                "Path %s should be marked unavailable" % rel_uri
            )

    def test_remove_busy_device(self):
        """Check that a busy device becomes indexed again."""

        files = self.create_test_data()

        with self.miner_fs.await_endpoint_added(self.device_path.as_uri()):
            self.add_removable_device(self.device_path)

        object_path = self.miner_fs.removable_device_object_path(self.device_path)
        endpoint_helper = self.helper_for_endpoint(object_path)

        for f in files:
            rel_uri = self.get_relative_uri(f)
            self.ensure_document_in_removable(endpoint_helper, rel_uri)
            assert self.data_source_available(endpoint_helper, rel_uri)

        with self.miner_fs.await_endpoint_removed(self.device_path.as_uri()):
            self.remove_removable_device(self.device_path, UnmountFlags.EMULATE_BUSY)

        uri = self.device_path.as_uri();
        result = None

        try:
            result = endpoint_helper.query('SELECT (1 AS ?u) {}')
        except Exception:
            pass

        self.assertIsNone(result)

        with self.miner_fs.await_endpoint_added(self.device_path.as_uri()):
            foo = 0

        for f in files:
            rel_uri = self.get_relative_uri(f)
            self.ensure_document_in_removable(endpoint_helper, rel_uri)
            assert self.data_source_available(endpoint_helper, rel_uri)


if __name__ == "__main__":
    fixtures.tracker_test_main()
