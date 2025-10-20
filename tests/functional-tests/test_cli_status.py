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
Test `localsearch status` subcommand
"""

from typing import *
import dataclasses
import pathlib
import subprocess
import os

import configuration
import fixtures
import shutil
import time

class TestCli(fixtures.TrackerCommandLineTestCase):
    def test_status(self):
        datadir = pathlib.Path(__file__).parent.joinpath("data/content")

        # Copy a file and wait for it to be indexed, in order to ensure idle state
        file = datadir.joinpath("text/mango.txt")
        target = pathlib.Path(os.path.join(self.indexed_dir, os.path.basename(file)))
        with self.await_document_inserted(target):
            shutil.copy(file, self.indexed_dir)

        output = self.run_cli(["localsearch", "status"])
        self.assertIn("1 file", output)
        self.assertIn("1 folder", output)
        self.assertIn("idle", output)

    def test_status_error(self):
        datadir = pathlib.Path(__file__).parent.joinpath("data/extractor-content")

        # Copy a photo, once as a PNG with JPEG extension, known to not be extracted
        file = datadir.joinpath("images/png-photo-1.png")
        target = pathlib.Path(os.path.join(self.indexed_dir, "png-photo-1.png"))
        target2 = pathlib.Path(os.path.join(self.indexed_dir, "png-photo-1.jpeg"))

        with self.await_photo_inserted(target):
            shutil.copy(file, target2)
            shutil.copy(file, target)

        # Check for the file with wrong extension
        output = self.run_cli(["localsearch", "status"])
        self.assertIn("png-photo-1.jpeg", output)
        self.assertIn("recorded failure", output)

    def test_status_term_noerror(self):
        datadir = pathlib.Path(__file__).parent.joinpath("data/content")

        # Copy a file and wait for it to be indexed, in order to ensure idle state
        file = datadir.joinpath("text/mango.txt")
        target = pathlib.Path(os.path.join(self.indexed_dir, os.path.basename(file)))
        with self.await_document_inserted(target):
            shutil.copy(file, self.indexed_dir)

        output = self.run_cli(["localsearch", "status", "mango"])
        self.assertIn("No reports found", output)

    def test_status_term_error(self):
        datadir = pathlib.Path(__file__).parent.joinpath("data/extractor-content")

        # Copy a photo, once as a PNG with JPEG extension, known to not be extracted
        file = datadir.joinpath("images/png-photo-1.png")
        target = pathlib.Path(os.path.join(self.indexed_dir, "png-photo-1.png"))
        bad_copy = pathlib.Path(os.path.join(self.indexed_dir, "png-photo-1.jpeg"))
        bad_copy2 = pathlib.Path(os.path.join(self.indexed_dir, "png-photo-2.jpeg"))

        with self.await_photo_inserted(target):
            shutil.copy(file, bad_copy)
            shutil.copy(file, bad_copy2)
            shutil.copy(file, target)

        # Check for a file with wrong extension
        output = self.run_cli(["localsearch", "status", "photo-1"])
        self.assertIn("png-photo-1.jpeg", output)
        self.assertIn("URI:", output)
        self.assertIn("Message:", output)

        # Check that the no args call lists the errors
        output = self.run_cli(["localsearch", "status"])
        self.assertIn("png-photo-1.jpeg", output)
        self.assertIn("png-photo-2.jpeg", output)

        # Check that deleting a file no longer lists its error
        os.remove(bad_copy2)
        output = self.run_cli(["localsearch", "status"])
        self.assertIn("png-photo-1.jpeg", output)
        self.assertNotIn("png-photo-2.jpeg", output)

    def test_status_stat(self):
        datadir = pathlib.Path(__file__).parent.joinpath("data/content")

        # Copy a file and wait for it to be indexed, in order to ensure state
        file = datadir.joinpath("text/mango.txt")
        target = pathlib.Path(os.path.join(self.indexed_dir, os.path.basename(file)))
        with self.await_document_inserted(target):
            shutil.copy(file, target)

        output = self.run_cli(["localsearch", "status", "--stat"])
        self.assertIn("nfo:Folder", output)
        self.assertIn("nfo:Document", output)
        self.assertIn("tracker:FileSystem", output)
        self.assertIn("tracker:Documents", output)

    def test_status_stat_term(self):
        datadir = pathlib.Path(__file__).parent.joinpath("data/content")

        # Copy a file and wait for it to be indexed, in order to ensure state
        file = datadir.joinpath("text/mango.txt")
        target = pathlib.Path(os.path.join(self.indexed_dir, os.path.basename(file)))
        with self.await_document_inserted(target):
            shutil.copy(file, target)

        output = self.run_cli(["localsearch", "status", "--stat", "folder"])
        self.assertIn("nfo:Folder", output)
        self.assertNotIn("nfo:Document", output)

    def test_status_follow(self):
        datadir = pathlib.Path(__file__).parent.joinpath("data/content")
        output = ""
        with subprocess.Popen(
                ["localsearch", "status", "--follow"],
                bufsize=1,
                text=True,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL) as proc:

            # Set up a temporary inhibition, this state should be reported in the output
            with subprocess.Popen(
                    ["localsearch", "inhibit", "cat"],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL) as inhibit_proc:
                time.sleep(3)
                # Close stdin to end the cat process
                inhibit_proc.stdin.close()

            # Copy a file and wait for it to be indexed
            file = datadir.joinpath("text/mango.txt")
            target = pathlib.Path(os.path.join(self.indexed_dir, os.path.basename(file)))
            with self.await_document_inserted(target):
                shutil.copy(file, target)

            proc.terminate()
            output = proc.stdout.read()

        self.assertIn("is paused", output)
        self.assertIn("Indexing", output)
        self.assertIn("Idle", output)

    def test_status_watch(self):
        datadir = pathlib.Path(__file__).parent.joinpath("data/content")
        output = ""
        with subprocess.Popen(
                ["localsearch", "status", "--watch"],
                bufsize=1,
                text=True,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL) as proc:

            # Copy a file and wait for it to be indexed
            file = datadir.joinpath("text/mango.txt")
            target = pathlib.Path(os.path.join(self.indexed_dir, os.path.basename(file)))
            with self.await_document_inserted(target):
                shutil.copy(file, target)

            proc.terminate()
            output = proc.stdout.read()

        # Check we've witnessed the file creation
        self.assertIn("mango.txt", output)
        self.assertIn("tracker:Documents", output)

    def test_status_wrongargs(self):
        err = None
        out = None
        try:
            # Pass unknown arg
            out = self.run_cli(["localsearch", "status", "--asdf"])
        except Exception as e:
            err = str(e)
        finally:
            self.assertIn("--asdf", err);
            self.assertIsNone(out)

if __name__ == "__main__":
    fixtures.tracker_test_main()
