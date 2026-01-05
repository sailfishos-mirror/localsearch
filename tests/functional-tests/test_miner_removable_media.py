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

import os
import stat
import time
from minerhelper import MinerFsHelper

log = logging.getLogger(__name__)


class MinerRemovableMediaTest(fixtures.TrackerMinerRemovableMediaTest):
    def setUp(self):
        super(MinerRemovableMediaTest, self).setUp()

        self.device_path = pathlib.Path(self.workdir).joinpath("removable-device-1")
        self.device_path.mkdir()

    def ensure_document_in_removable(self, endpoint_helper, uri):
        return endpoint_helper.ensure_resource(
            fixtures.DOCUMENTS_GRAPH, f"nie:isStoredAs <{uri}>",
        )

    def assertResourceExists(self, helper, urn):
        if helper.ask("ASK { <%s> a rdfs:Resource }" % urn) == False:
            self.fail(f"Resource <{urn}> does not exist")

    def assertResourceMissing(self, helper, urn):
        if helper.ask("ASK { <%s> a rdfs:Resource }" % urn) == True:
            self.fail(f"Resource <{urn}> should not exist")

    def create_test_data(self):
        files = ["file1.txt", "dir1/file2.txt"]

        for f in files:
            path = self.device_path.joinpath(f)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("This file exists.")

        return files


class MinerRemovableTest(MinerRemovableMediaTest):
    """Tests for localsearch with index-removable-devices feature."""

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
            self.assertResourceExists(endpoint_helper, rel_uri)

        with self.miner_fs.await_endpoint_removed(self.device_path.as_uri()):
            self.remove_removable_device(self.device_path)

        endpoint_helper = None
        try:
            endpoint_helper = self.helper_for_endpoint(object_path)
        except:
            pass

        assert not endpoint_helper

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
            self.assertResourceExists(endpoint_helper, rel_uri)

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
            self.assertResourceExists(endpoint_helper, rel_uri)


    def test_readonly(self):
        """Device should not be indexed by Tracker when connected."""

        files = self.create_test_data()

        # Mangle database location
        dbpath = self.device_path.joinpath('.localsearch3')
        dbpath.write_text("I'm not a database")

        object_path = self.miner_fs.removable_device_object_path (self.device_path)
        self.add_removable_device(self.device_path)

        time.sleep(2)

        endpoint_helper = None
        try:
            endpoint_helper = self.helper_for_endpoint(object_path)
        except:
            pass

        assert not endpoint_helper


class MinerRemovableConfigTest(MinerRemovableMediaTest):
    def setUp(self):
        super(MinerRemovableConfigTest, self).setUp()
        self.offline_test = True

    def maybeStopService(self):
        if self.offline_test:
            self.sandbox.stop_daemon('org.freedesktop.LocalSearch3')

    def maybeStartService(self):
        if self.offline_test:
            self.miner_fs = MinerFsHelper(self.sandbox.get_session_bus_connection())
            self.miner_fs.start()
            self.add_removable_device(self.device_path)

    def test_ignored_files(self):
        files = self.create_test_data()

        object_path = self.miner_fs.removable_device_object_path (self.device_path)
        self.add_removable_device(self.device_path)
        self.miner_fs.await_endpoint_added(self.device_path.as_uri())
        endpoint_helper = self.helper_for_endpoint(object_path)

        self.ensure_document_in_removable(
            endpoint_helper, self.get_relative_uri('file1.txt'))

        filename1 = "file3.txt"
        rel_uri1 = self.get_relative_uri(filename1);
        path1 = self.device_path.joinpath(filename1)
        uri1 = self.uri(filename1)

        with endpoint_helper.await_insert(
            fixtures.DOCUMENTS_GRAPH, f"nie:isStoredAs <{rel_uri1}>"
        ):
            with open(path1, "w") as f:
                f.write("Foo bar baz")

        self.assertResourceExists(endpoint_helper, rel_uri1)
        resource_id = endpoint_helper.get_content_resource_id(rel_uri1)

        dconf = self.sandbox.get_dconf_client()

        # Set ignored-files filter, file should disappear
        with endpoint_helper.await_delete(fixtures.DOCUMENTS_GRAPH, resource_id):
            self.maybeStopService();
            dconf.write (
                'org.freedesktop.Tracker3.Miner.Files',
                'ignored-files', GLib.Variant.new_strv(['*.txt']))
            self.maybeStartService();

        # Newly created filtered file should not appear either
        filename2 = "file4.txt"
        rel_uri2 = self.get_relative_uri (filename2)
        path2 = self.device_path.joinpath(filename2)
        uri2 = self.uri(filename2)
        with open(path2, "w") as f:
            f.write("Foo bar baz")

        time.sleep(2)
        self.assertResourceMissing(endpoint_helper, rel_uri2)

        # Ensure that both files are back by removing the filter
        with endpoint_helper.await_insert(
            fixtures.DOCUMENTS_GRAPH, f"nie:isStoredAs <{rel_uri1}>"
        ):
            self.maybeStopService();
            dconf.write (
                'org.freedesktop.Tracker3.Miner.Files',
                'ignored-files', GLib.Variant.new_strv([]))
            self.maybeStartService();

        self.ensure_document_in_removable(endpoint_helper, rel_uri2)

    def test_ignored_directories(self):
        object_path = self.miner_fs.removable_device_object_path (self.device_path)
        self.add_removable_device(self.device_path)
        self.miner_fs.await_endpoint_added(self.device_path.as_uri())
        endpoint_helper = self.helper_for_endpoint(object_path)

        dirname = "dir"
        dir_rel_uri = self.get_relative_uri(dirname);

        filename = "dir/file3.txt"
        rel_uri = self.get_relative_uri(filename);
        path = self.device_path.joinpath(filename)

        with endpoint_helper.await_insert(
            fixtures.DOCUMENTS_GRAPH, f"nie:isStoredAs <{rel_uri}>"
        ):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("Foo bar baz")

        self.assertResourceExists(endpoint_helper, dir_rel_uri)
        resource_id = endpoint_helper.get_content_resource_id(dir_rel_uri)

        dconf = self.sandbox.get_dconf_client()

        # Set ignored-directories filter, directory and file should disappear
        with endpoint_helper.await_delete(fixtures.FILESYSTEM_GRAPH, resource_id):
            self.maybeStopService();
            dconf.write (
                'org.freedesktop.Tracker3.Miner.Files',
                'ignored-directories', GLib.Variant.new_strv(['dir']))
            self.maybeStartService();

        self.assertResourceMissing(endpoint_helper, dir_rel_uri)
        self.assertResourceMissing(endpoint_helper, rel_uri)

        # Ensure the file/dir are back after removing the filter
        with endpoint_helper.await_insert(
            fixtures.DOCUMENTS_GRAPH, f"nie:isStoredAs <{rel_uri}>"
        ):
            self.maybeStopService();
            dconf.write (
                'org.freedesktop.Tracker3.Miner.Files',
                'ignored-directories', GLib.Variant.new_strv([]))
            self.maybeStartService();

        self.assertResourceExists(endpoint_helper, dir_rel_uri)
        self.assertResourceExists(endpoint_helper, rel_uri)

    def test_ignored_directories_with_content(self):
        object_path = self.miner_fs.removable_device_object_path (self.device_path)
        self.add_removable_device(self.device_path)
        self.miner_fs.await_endpoint_added(self.device_path.as_uri())
        endpoint_helper = self.helper_for_endpoint(object_path)

        dirname = "dir"
        dir_rel_uri = self.get_relative_uri(dirname);

        filename = "dir/file3.txt"
        rel_uri = self.get_relative_uri(filename);
        path = self.device_path.joinpath(filename)

        hidden = self.device_path.joinpath("dir/hide-me")

        with endpoint_helper.await_insert(
            fixtures.DOCUMENTS_GRAPH, f"nie:isStoredAs <{rel_uri}>"
        ):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("Foo bar baz")
            hidden.write_text("")

        self.assertResourceExists(endpoint_helper, dir_rel_uri)
        self.assertResourceExists(endpoint_helper, rel_uri)
        resource_id = endpoint_helper.get_content_resource_id(rel_uri)

        dconf = self.sandbox.get_dconf_client()

        # Set ignored-directories-with-content filter, dir and file should disappear
        with endpoint_helper.await_delete(fixtures.DOCUMENTS_GRAPH, resource_id):
            self.maybeStopService();
            dconf.write (
                'org.freedesktop.Tracker3.Miner.Files',
                'ignored-directories-with-content', GLib.Variant.new_strv(['hide-me']))
            self.maybeStartService();

        self.assertResourceMissing(endpoint_helper, dir_rel_uri)
        self.assertResourceMissing(endpoint_helper, rel_uri)

        # Ensure the file/dir are back after removing the filter
        with endpoint_helper.await_insert(
            fixtures.DOCUMENTS_GRAPH, f"nie:isStoredAs <{rel_uri}>"
        ):
            self.maybeStopService();
            dconf.write (
                'org.freedesktop.Tracker3.Miner.Files',
                'ignored-directories-with-content', GLib.Variant.new_strv([]))
            self.maybeStartService();

        self.assertResourceExists(endpoint_helper, dir_rel_uri)
        self.assertResourceExists(endpoint_helper, rel_uri)


class MinerRemovableLiveConfigChangeTest(MinerRemovableConfigTest):
    """Tests for config changes affecting removable devices."""
    def setUp(self):
        super(MinerRemovableLiveConfigChangeTest, self).setUp()
        self.offline_test = False

    def test_index_removable_devices(self):
        dconf = self.sandbox.get_dconf_client()
        dconf.write (
            'org.freedesktop.Tracker3.Miner.Files',
            'index-removable-devices', GLib.Variant.new_boolean(False))

        path = self.device_path.joinpath("file1.txt")
        path.write_text("Foo bar baz")
        self.add_removable_device(self.device_path)

        # Ensure the file was not indexed
        time.sleep(2)

        object_path = self.miner_fs.removable_device_object_path (self.device_path)
        endpoint_helper = None

        try:
            endpoint_helper = self.helper_for_endpoint(object_path)
        except:
            pass

        assert not endpoint_helper

        # Mountpoint should be indexed with the config change
        with self.miner_fs.await_endpoint_added(self.device_path.as_uri()):
            dconf.write (
                'org.freedesktop.Tracker3.Miner.Files',
                'index-removable-devices', GLib.Variant.new_boolean(True))

        # The "device" should be "removed", with the config change
        with self.miner_fs.await_endpoint_removed(self.device_path.as_uri()):
            dconf.write (
                'org.freedesktop.Tracker3.Miner.Files',
                'index-removable-devices', GLib.Variant.new_boolean(False))


if __name__ == "__main__":
    fixtures.tracker_test_main()
