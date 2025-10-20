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
Test `localsearch extract` subcommand
"""

from typing import *
import dataclasses
import pathlib
import os
import re

import configuration
import fixtures
import shutil


class TestCli(fixtures.TrackerCommandLineTestCase):
    def test_extract_noargs(self):
        out = ""
        err = ""
        try:
            out = self.run_cli(["localsearch", "extract"])
        except Exception as e:
            err = str(e)

        self.assertEqual("", out)
        self.assertIn("CLI command failed", err)

    def test_extract_help(self):
        out = self.run_cli(["localsearch", "extract", "--help"])
        self.assertIn("Usage", out)

    def test_extract_wrongargs(self):
        out = ""
        err = ""
        try:
            out = self.run_cli(["localsearch", "extract", "--asdf"])
        except Exception as e:
            err = str(e)

        self.assertEqual("", out)
        self.assertIn("CLI command failed", err)

    def test_extract_nonexistent(self):
        out = ""
        err = ""
        try:
            out = self.run_cli(["localsearch", "extract", "/.abc"])
        except Exception as e:
            err = str(e)

        self.assertEqual("", out)
        self.assertIn("Metadata extraction failed", err)

if __name__ == "__main__":
    fixtures.tracker_test_main()
