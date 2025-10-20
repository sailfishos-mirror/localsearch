# Copyright (C) 2025, Red Hat Inc.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
# 02110-1301, USA.
#
# Author: Carlos Garnacho <carlosg@gnome.org>

"""
Test `localsearch index` subcommand
"""

import gi
from gi.repository import GLib

from typing import *
import dataclasses
import pathlib
import os

import configuration
import fixtures
import shutil

from trackertestutils.dconf import DConfClient
import configuration as cfg

class TestCli(fixtures.TrackerCommandLineTestCase):
    def environment(self):
        extra_env = super(TestCli, self).environment()
        extra_env["HOME"] = self.indexed_dir
        return extra_env

    def test_index(self):
        output = self.run_cli(["localsearch", "index"])
        self.assertIn("test-monitored", output)
        self.assertIn("test-non-recursive", output)

    def test_index_help(self):
        output = self.run_cli(["localsearch", "index", "--help"])
        self.assertIn("Usage", output)

    def test_index_remove_add(self):
        datadir = pathlib.Path(__file__).parent.joinpath("data/content")

        self.tracker.ensure_resource(
            fixtures.FILESYSTEM_GRAPH,
            f"nie:isStoredAs <{self.uri(self.indexed_dir)}>",
            timeout=cfg.AWAIT_TIMEOUT,
        )
        resource_id = self.tracker.get_content_resource_id(self.uri(self.indexed_dir))

        with self.tracker.await_delete(
            fixtures.FILESYSTEM_GRAPH, resource_id, timeout=cfg.AWAIT_TIMEOUT
        ):
            self.run_cli(["localsearch", "index", "--remove", self.indexed_dir])
            output = self.run_cli(["localsearch", "index"])
            self.assertNotIn("test-monitored", output)

        file = datadir.joinpath("text/mango.txt")
        target = pathlib.Path(os.path.join(self.indexed_dir, os.path.basename(file)))
        shutil.copy(file, self.indexed_dir)

        # Add the directory back, and wait for the new file to be indexed
        with self.await_document_inserted(target):
            self.run_cli(["localsearch", "index", "--recursive", "--add", self.indexed_dir])

    def test_index_alias(self):
        dconf = self.sandbox.get_dconf_client()
        dconf.write (
            'org.freedesktop.Tracker3.Miner.Files',
            'index-recursive-directories', GLib.Variant.new_strv([]))
        dconf.write (
            'org.freedesktop.Tracker3.Miner.Files',
            'index-single-directories', GLib.Variant.new_strv(['&DESKTOP']))
        output1 = self.run_cli(["localsearch", "index"])
        desktop_path = self.path("test-monitored/Desktop")
        self.assertIn("/Desktop", output1)

        # Ensure that things stay the same if adding the path explicitly
        self.run_cli(["localsearch", "index", "--add", desktop_path])
        output2 = self.run_cli(["localsearch", "index"])
        self.assertEqual(output1, output2)

        # Ensure that removing by path works
        self.run_cli(["localsearch", "index", "--remove", desktop_path])
        output3 = self.run_cli(["localsearch", "index"])
        self.assertNotIn("/Desktop", output3)

    def test_index_envvar(self):
        dconf = self.sandbox.get_dconf_client()
        dconf.write (
            'org.freedesktop.Tracker3.Miner.Files',
            'index-recursive-directories', GLib.Variant.new_strv([]))
        dconf.write (
            'org.freedesktop.Tracker3.Miner.Files',
            'index-single-directories', GLib.Variant.new_strv(['$HOME']))
        output1 = self.run_cli(["localsearch", "index"])
        self.assertIn("test-monitored", output1)

        # Ensure that things stay the same if adding the path explicitly
        self.run_cli(["localsearch", "index", "--add", self.indexed_dir])
        output2 = self.run_cli(["localsearch", "index"])
        self.assertEqual(output1, output2)

        # Ensure that removing by path works
        self.run_cli(["localsearch", "index", "--remove", self.indexed_dir])
        output3 = self.run_cli(["localsearch", "index"])
        self.assertNotIn("test-monitored", output3)

    def test_index_idempotent_add(self):
        output1 = self.run_cli(["localsearch", "index"])
        self.assertIn("test-monitored", output1)
        self.run_cli(["localsearch", "index", "--recursive", "--add", self.indexed_dir])
        output2 = self.run_cli(["localsearch", "index"])
        self.assertEqual(output1, output2)

    def test_index_idempotent_remove(self):
        output1 = self.run_cli(["localsearch", "index"])
        self.assertIn("test-monitored", output1)
        # Remove non indexed path
        self.run_cli(["localsearch", "index", "--remove", "/abc"])
        output2 = self.run_cli(["localsearch", "index"])
        self.assertEqual(output1, output2)

    def test_index_wrongargs(self):
        err = None
        out = None
        try:
            # Pass unknown arg
            out = self.run_cli(["localsearch", "index", "--asdf"])
        except Exception as e:
            err = str(e)
        finally:
            self.assertIn("--asdf", err);
            self.assertIsNone(out)

    def test_index_wrongargs2(self):
        err = None
        out = None
        try:
            # Pass pass both --add and --remove
            out = self.run_cli(["localsearch", "index", "--add", "--remove", self.indexed_dir])
        except Exception as e:
            err = str(e)
        finally:
            self.assertIn("mutually exclusive", err);
            self.assertIsNone(out)

    def test_index_add_noargs(self):
        err = None
        out = None
        try:
            # Pass bare --add with no arguments
            out = self.run_cli(["localsearch", "index", "--add"])
        except Exception as e:
            err = str(e)
        finally:
            self.assertIn("Please specify", err);
            self.assertIsNone(out)

    def test_index_remove_noargs(self):
        err = None
        out = None
        try:
            # Pass bare --remove with no arguments
            out = self.run_cli(["localsearch", "index", "--remove"])
        except Exception as e:
            err = str(e)
        finally:
            self.assertIn("Please specify", err);
            self.assertIsNone(out)

    def test_index_recursive_noargs(self):
        err = None
        out = None
        try:
            # Pass bare --recursive with no more arguments
            out = self.run_cli(["localsearch", "index", "--recursive"])
        except Exception as e:
            err = str(e)
        finally:
            self.assertIn("Please specify", err);
            self.assertIsNone(out)

    def test_index_recursive_wrongargs(self):
        err = None
        out = None
        try:
            # Pass bare --recursive with no more specifiers
            out = self.run_cli(["localsearch", "index", "--recursive", "/tmp"])
        except Exception as e:
            err = str(e)
        finally:
            self.assertIn("Either --add or --remove", err);
            self.assertIsNone(out)

    def test_index_recursive_wrongargs2(self):
        err = None
        out = None
        try:
            # Pass --recursive with --remove
            out = self.run_cli(["localsearch", "index", "--recursive", "--remove", self.indexed_dir])
        except Exception as e:
            err = str(e)
        finally:
            self.assertIn("requires --add", err);
            self.assertIsNone(out)

if __name__ == "__main__":
    fixtures.tracker_test_main()
