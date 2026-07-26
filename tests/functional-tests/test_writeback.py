# Copyright (C) 2026, Carlos Garnacho (carlosg@gnome.org)
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

from gi.repository import GLib, Tsparql

class WritebackTest(fixtures.TrackerWritebackTest):
    def test_non_existent(self):
        err = ''
        try:
            resource = self.create_resource("nfo:Audio", pathlib.Path("/nonexistent"), {})
            self.writeback_data(resource.serialize())
        except Exception as e:
            err = str(e)
        finally:
            self.assertIn("doesn't exist", err)

    def test_non_writable(self):
        err = ''
        try:
            resource = self.create_resource("nfo:Audio", pathlib.Path("/proc/stat"), {})
            self.writeback_data(resource.serialize())
        except Exception as e:
            err = str(e)
        finally:
            self.assertIn("not writable", err)

    def test_no_type(self):
        err = ''
        try:
            resource = Tsparql.Resource.new('file:///foo')
            self.writeback_data(resource.serialize())
        except Exception as e:
            err = str(e)
        finally:
            self.assertIn("does not define rdf:type", err)

    def test_unhandled_type(self):
        err = ''
        try:
            resource = Tsparql.Resource.new('file:///foo')
            resource.add_uri('rdf:type', 'nmm:Video')
            self.writeback_data(resource.serialize())
        except Exception as e:
            err = str(e)
        finally:
            self.assertIn("does not match any writeback modules", err)

    def test_no_isStoredAs(self):
        err = ''
        try:
            resource = Tsparql.Resource.new('file:///foo')
            resource.add_uri('rdf:type', 'nfo:Audio')
            self.writeback_data(resource.serialize())
        except Exception as e:
            err = str(e)
        finally:
            self.assertIn("does not contain nie:isStoredAs", err)

    def test_bad_data(self):
        err = ''
        try:
            data = GLib.Variant('a{sv}', {
                # Object path types are unused in resource serialization
                'foo': GLib.Variant('o', '/foo'),
            })
            self.writeback_data(data)
        except Exception as e:
            err = str(e)
        finally:
            self.assertIn("does not serialize to a resource", err)


if __name__ == "__main__":
    fixtures.tracker_test_main()
