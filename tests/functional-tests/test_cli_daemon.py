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
Test `localsearch daemon` subcommand
"""

from typing import *
import dataclasses
import pathlib
import os
import time

import configuration
import fixtures
import shutil

class TestCli(fixtures.TrackerCommandLineTestCase):
    def test_daemon_terminate_start(self):
        datadir = pathlib.Path(__file__).parent.joinpath("data/content")

        # Copy a file and wait for it to be indexed, in order to ensure idle state
        file = datadir.joinpath("text/mango.txt")
        target = pathlib.Path(os.path.join(self.indexed_dir, os.path.basename(file)))
        with self.await_document_inserted(target):
            shutil.copy(file, self.indexed_dir)

        pid = self.sandbox.session_bus.get_connection_unix_process_id_sync(
            'org.freedesktop.LocalSearch3')

        # Terminate the indexer
        self.run_cli(["localsearch", "daemon", "--terminate"])

        # Ensure the process is gone
        pid_exists = True
        attempts = 0
        while pid_exists:
            if attempts == 10:
                raise RuntimeError("Took too long to stop indexer")

            try:
                program = os.readlink("/proc/" + str(pid) + "/exe")
                pid_exists = os.path.basename(program) == "localsearch-3"
                if pid_exists:
                    time.sleep(1)
                    attempts += 1
            except:
                pid_exists = False

        # State should be none
        output = self.run_cli(["localsearch", "daemon"])
        self.assertEqual("", output)

        # Re-start the indexer and trigger another change
        self.run_cli(["localsearch", "daemon", "--start"])

        file = datadir.joinpath("text/Document 2.txt")
        target = pathlib.Path(os.path.join(self.indexed_dir, os.path.basename(file)))
        with self.await_document_inserted(target):
            shutil.copy(file, self.indexed_dir)

        # State should be idle now
        output = self.run_cli(["localsearch", "daemon"])
        self.assertIn("Idle", output)

    def test_daemon_noargs(self):
        datadir = pathlib.Path(__file__).parent.joinpath("data/content")

        # Copy a file and wait for it to be indexed, in order to ensure state
        file = datadir.joinpath("text/mango.txt")
        target = pathlib.Path(os.path.join(self.indexed_dir, os.path.basename(file)))
        with self.await_document_inserted(target):
            shutil.copy(file, self.indexed_dir)

        output = self.run_cli(["localsearch", "daemon"])
        # State should be idle
        self.assertIn("Idle", output)

    def test_daemon_wrongargs(self):
        err = None
        out = None
        try:
            # Pass unknown arg
            out = self.run_cli(["localsearch", "daemon", "--asdf"])
        except Exception as e:
            err = str(e)
        finally:
            self.assertIn("--asdf", err);
            self.assertIsNone(out)

if __name__ == "__main__":
    fixtures.tracker_test_main()
