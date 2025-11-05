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

    # Missing metadata:
    # nmm:artwork, nmm:internationalStandardRecordingCode,
    # nmm:lyrics, nie:description, nao:hasTag

    # MP3

    def test_mp3_title(self):
        self._writeback_test(
            "writeback-test-5.mp3",
            {"nie:title": "test_title"})

    def test_mp3_comment(self):
        self._writeback_test(
            "writeback-test-5.mp3",
            {"nie:comment": "test_comment"})

    def test_mp3_genre(self):
        self._writeback_test(
            "writeback-test-5.mp3",
            {"nfo:genre": "test_genre"})

    def test_mp3_artist(self):
        self._writeback_test(
            "writeback-test-5.mp3",
            {"nmm:artist": {"nmm:artistName": "test_artist",
                            "tracker:hasExternalReference": {"tracker:referenceSource": "https://musicbrainz.org/doc/Artist",
                                                             "tracker:referenceIdentifier": "test_mb_identifier"}}})

    @unittest.skip('gstreamer does not write the tag')
    def test_mp3_mb_track(self):
        self._writeback_test(
            "writeback-test-5.mp3",
            {"tracker:hasExternalReference": [{"tracker:referenceSource": "https://musicbrainz.org/doc/Recording",
                                               "tracker:referenceIdentifier": "test_mb_recording"},
                                              {"tracker:referenceSource": "https://musicbrainz.org/doc/Track",
                                               "tracker:referenceIdentifier": "test_mb_track"}]})

    def test_mp3_track_number(self):
        self._writeback_test(
            "writeback-test-5.mp3",
            {"nmm:trackNumber": 42})

    @unittest.skip('gstreamer does not write the tag')
    def test_mp3_content_created(self):
        self._writeback_test(
            "writeback-test-5.mp3",
            {"nie:contentCreated": "1234-12-23T00:00:00Z"})

    @unittest.skip('gstreamer does not write the tag')
    def test_mp3_isrc(self):
        self._writeback_test(
            "writeback-test-5.mp3",
            {"nmm:internationalStandardRecordingCode": "test_isrc"})

    def test_mp3_composer(self):
        self._writeback_test(
            "writeback-test-5.mp3",
            {"nmm:composer": {"nmm:artistName": "test_composer"}})

    @unittest.skip('gstreamer does not write the tag')
    def test_mp3_publisher(self):
        self._writeback_test(
            "writeback-test-5.mp3",
            {"nco:publisher": {"nco:fullname": "test_publisher"}})

    @unittest.skip('gstreamer does not write the tag')
    def test_mp3_hasHash (self):
        self._writeback_test (
            "writeback-test-5.mp3",
            {"nfo:hasHash" : {"nfo:hashAlgorithm": "chromaprint",
                              "nfo:hashValue": "test_hash"}})

    def test_mp3_music_album(self):
        self._writeback_test(
            "writeback-test-5.mp3",
            {"nmm:musicAlbum": {"nie:title": "test_title",
                                "nmm:albumArtist": { "nmm:artistName": "test_artist"},
                                "tracker:hasExternalReference": [{"tracker:referenceSource": "https://musicbrainz.org/doc/Release",
                                                                  "tracker:referenceIdentifier": "test_mb_release"},
                                                                 {"tracker:referenceSource": "https://musicbrainz.org/doc/Release_Group",
                                                                  "tracker:referenceIdentifier": "test_mb_release_group"}]},
             "nmm:musicAlbumDisc": {"nmm:setNumber": 42}})

    # Ogg

    def test_ogg_title(self):
        self._writeback_test(
            "writeback-test-6.ogg",
            {"nie:title": "test_title"})

    @unittest.skip('gstreamer does not write the tag')
    def test_ogg_comment(self):
        self._writeback_test(
            "writeback-test-6.ogg",
            {"nie:comment": "test_comment"})

    def test_ogg_genre(self):
        self._writeback_test(
            "writeback-test-6.ogg",
            {"nfo:genre": "test_genre"})

    def test_ogg_artist(self):
        self._writeback_test(
            "writeback-test-6.ogg",
            {"nmm:artist": {"nmm:artistName": "test_artist",
                            "tracker:hasExternalReference": {"tracker:referenceSource": "https://musicbrainz.org/doc/Artist",
                                                             "tracker:referenceIdentifier": "test_mb_identifier"}}})

    def test_ogg_mb_track(self):
        self._writeback_test(
            "writeback-test-6.ogg",
            {"tracker:hasExternalReference": [{"tracker:referenceSource": "https://musicbrainz.org/doc/Recording",
                                               "tracker:referenceIdentifier": "test_mb_recording"},
                                              {"tracker:referenceSource": "https://musicbrainz.org/doc/Track",
                                               "tracker:referenceIdentifier": "test_mb_track"}]})

    def test_ogg_track_number(self):
        self._writeback_test(
            "writeback-test-6.ogg",
            {"nmm:trackNumber": 42})

    def test_ogg_content_created(self):
        self._writeback_test(
            "writeback-test-6.ogg",
            {"nie:contentCreated": "1234-12-23T00:00:00Z"})

    @unittest.skip('gstreamer does not write the tag')
    def test_ogg_isrc(self):
        self._writeback_test(
            "writeback-test-6.ogg",
            {"nmm:internationalStandardRecordingCode": "test_isrc"})

    def test_ogg_composer(self):
        self._writeback_test(
            "writeback-test-6.ogg",
            {"nmm:composer": {"nmm:artistName": "test_composer"}})

    @unittest.skip('gstreamer does not write the tag')
    def test_ogg_publisher(self):
        self._writeback_test(
            "writeback-test-6.ogg",
            {"nco:publisher": {"nco:fullname": "test_publisher"}})

    def test_ogg_hasHash (self):
        self._writeback_test (
            "writeback-test-6.ogg",
            {"nfo:hasHash" : {"nfo:hashAlgorithm": "chromaprint",
                              "nfo:hashValue": "test_hash"}})

    def test_ogg_music_album(self):
        self._writeback_test(
            "writeback-test-6.ogg",
            {"nmm:musicAlbum": {"nie:title": "test_title",
                                "nmm:albumArtist": { "nmm:artistName": "test_artist"},
                                "tracker:hasExternalReference": [{"tracker:referenceSource": "https://musicbrainz.org/doc/Release",
                                                                  "tracker:referenceIdentifier": "test_mb_release"},
                                                                 {"tracker:referenceSource": "https://musicbrainz.org/doc/Release_Group",
                                                                  "tracker:referenceIdentifier": "test_mb_release_group"}]},
             "nmm:musicAlbumDisc": {"nmm:setNumber": 42}})

    # Flac

    def test_flac_title(self):
        self._writeback_test(
            "writeback-test-7.flac",
            {"nie:title": "test_title"})

    @unittest.skip('gstreamer does not write the tag')
    def test_flac_comment(self):
        self._writeback_test(
            "writeback-test-7.flac",
            {"nie:comment": "test_comment"})

    def test_flac_genre(self):
        self._writeback_test(
            "writeback-test-7.flac",
            {"nfo:genre": "test_genre"})

    def test_flac_artist(self):
        self._writeback_test(
            "writeback-test-7.flac",
            {"nmm:artist": {"nmm:artistName": "test_artist",
                            "tracker:hasExternalReference": {"tracker:referenceSource": "https://musicbrainz.org/doc/Artist",
                                                             "tracker:referenceIdentifier": "test_mb_identifier"}}})

    def test_flac_mb_track(self):
        self._writeback_test(
            "writeback-test-7.flac",
            {"tracker:hasExternalReference": [{"tracker:referenceSource": "https://musicbrainz.org/doc/Recording",
                                               "tracker:referenceIdentifier": "test_mb_recording"},
                                              {"tracker:referenceSource": "https://musicbrainz.org/doc/Track",
                                               "tracker:referenceIdentifier": "test_mb_track"}]})

    def test_flac_track_number(self):
        self._writeback_test(
            "writeback-test-7.flac",
            {"nmm:trackNumber": 42})

    def test_flac_content_created(self):
        self._writeback_test(
            "writeback-test-7.flac",
            {"nie:contentCreated": "1234-12-23T00:00:00Z"})

    @unittest.skip('gstreamer does not write the tag')
    def test_flac_isrc(self):
        self._writeback_test(
            "writeback-test-7.flac",
            {"nmm:internationalStandardRecordingCode": "test_isrc"})

    def test_flac_composer(self):
        self._writeback_test(
            "writeback-test-7.flac",
            {"nmm:composer": {"nmm:artistName": "test_composer"}})

    @unittest.skip('gstreamer does not write the tag')
    def test_flac_publisher(self):
        self._writeback_test(
            "writeback-test-7.flac",
            {"nco:publisher": {"nco:fullname": "test_publisher"}})

    def test_flac_hasHash (self):
        self._writeback_test (
            "writeback-test-7.flac",
            {"nfo:hasHash" : {"nfo:hashAlgorithm": "chromaprint",
                              "nfo:hashValue": "test_hash"}})

    def test_flac_music_album(self):
        self._writeback_test(
            "writeback-test-7.flac",
            {"nmm:musicAlbum": {"nie:title": "test_title",
                                "nmm:albumArtist": { "nmm:artistName": "test_artist"},
                                "tracker:hasExternalReference": [{"tracker:referenceSource": "https://musicbrainz.org/doc/Release",
                                                                  "tracker:referenceIdentifier": "test_mb_release"},
                                                                 {"tracker:referenceSource": "https://musicbrainz.org/doc/Release_Group",
                                                                  "tracker:referenceIdentifier": "test_mb_release_group"}]},
             "nmm:musicAlbumDisc": {"nmm:setNumber": 42}})

    # AAC/MP4

    def test_aac_title(self):
        self._writeback_test(
            "writeback-test-8.mp4",
            {"nie:title": "test_title"})

    def test_aac_comment(self):
        self._writeback_test(
            "writeback-test-8.mp4",
            {"nie:comment": "test_comment"})

    def test_aac_genre(self):
        self._writeback_test(
            "writeback-test-8.mp4",
            {"nfo:genre": "test_genre"})

    @unittest.skip('gstreamer does not write the tag')
    def test_aac_artist(self):
        self._writeback_test(
            "writeback-test-8.mp4",
            {"nmm:artist": {"nmm:artistName": "test_artist",
                            "tracker:hasExternalReference": {"tracker:referenceSource": "https://musicbrainz.org/doc/Artist",
                                                             "tracker:referenceIdentifier": "test_mb_identifier"}}})

    @unittest.skip('gstreamer does not write the tag')
    def test_aac_mb_track(self):
        self._writeback_test(
            "writeback-test-8.mp4",
            {"tracker:hasExternalReference": [{"tracker:referenceSource": "https://musicbrainz.org/doc/Recording",
                                               "tracker:referenceIdentifier": "test_mb_recording"},
                                              {"tracker:referenceSource": "https://musicbrainz.org/doc/Track",
                                               "tracker:referenceIdentifier": "test_mb_track"}]})

    def test_aac_track_number(self):
        self._writeback_test(
            "writeback-test-8.mp4",
            {"nmm:trackNumber": 42})

    def test_aac_content_created(self):
        self._writeback_test(
            "writeback-test-8.mp4",
            {"nie:contentCreated": "1234-12-23T00:00:00.000000Z"})

    @unittest.skip('gstreamer does not write the tag')
    def test_aac_isrc(self):
        self._writeback_test(
            "writeback-test-8.mp4",
            {"nie:title": "test_title",
             "nmm:internationalStandardRecordingCode": "test_isrc"})

    def test_aac_composer(self):
        self._writeback_test(
            "writeback-test-8.mp4",
            {"nmm:composer": {"nmm:artistName": "test_composer"}})

    @unittest.skip('gstreamer does not write the tag')
    def test_aac_publisher(self):
        self._writeback_test(
            "writeback-test-8.mp4",
            {"nco:publisher": {"nco:fullname": "test_publisher"}})

    @unittest.skip('gstreamer does not write the tag')
    def test_aac_hasHash (self):
        self._writeback_test (
            "writeback-test-8.mp4",
            {"nfo:hasHash" : {"nfo:hashAlgorithm": "chromaprint",
                              "nfo:hashValue": "test_hash"}})

    @unittest.skip('gstreamer does not write the tag')
    def test_aac_music_album(self):
        self._writeback_test(
            "writeback-test-8.mp4",
            {"nmm:musicAlbum": {"nie:title": "test_title",
                                "nmm:albumArtist": { "nmm:artistName": "test_artist"},
                                "tracker:hasExternalReference": [{"tracker:referenceSource": "https://musicbrainz.org/doc/Release",
                                                                  "tracker:referenceIdentifier": "test_mb_release"},
                                                                 {"tracker:referenceSource": "https://musicbrainz.org/doc/Release_Group",
                                                                  "tracker:referenceIdentifier": "test_mb_release_group"}]},
             "nmm:musicAlbumDisc": {"nmm:setNumber": 42}})


if __name__ == "__main__":
    fixtures.tracker_test_main()
