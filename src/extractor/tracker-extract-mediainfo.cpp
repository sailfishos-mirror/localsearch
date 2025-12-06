/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 * Author: Carlos Garnacho <carlosg@gnome.org>
 */

#include "config-miners.h"

#include <glib.h>

#include <MediaInfo/MediaInfo.h>

#include <tracker-common.h>
#include "tracker-cue-sheet.h"
#include "utils/tracker-extract.h"

#ifdef HAVE_EXEMPI
#include "utils/tracker-xmp.h"
#endif

#ifdef HAVE_GUPNP_DLNA
#include "tracker-gupnp-mediainfo.h"
#endif

#define CHUNK_N_BYTES (2 << 15)

using namespace MediaInfoLib;

typedef void (* MappingFunc) (TrackerResource *resource,
                              const char      *property,
                              const char      *value,
                              MediaInfo       *ctx,
                              gpointer         user_data);

static void map_to_int (TrackerResource *resource,
                        const char      *property,
                        const char      *value,
                        MediaInfo       *ctx,
                        gpointer         user_data);

static void map_msec_to_sec (TrackerResource *resource,
                             const char      *property,
                             const char      *value,
                             MediaInfo       *ctx,
                             gpointer         user_data);

static void map_to_double (TrackerResource *resource,
                           const char      *property,
                           const char      *value,
                           MediaInfo       *ctx,
                           gpointer         user_data);

static void map_to_datetime (TrackerResource *resource,
                             const char      *property,
                             const char      *value,
                             MediaInfo       *ctx,
                             gpointer         user_data);

static void map_to_artist (TrackerResource *resource,
                           const char      *property,
                           const char      *value,
                           MediaInfo       *ctx,
                           gpointer         user_data);

static void map_to_composer_ogg (TrackerResource *resource,
                                 const char      *property,
                                 const char      *value,
                                 MediaInfo       *ctx,
                                 gpointer         user_data);

static void map_to_composer (TrackerResource *resource,
                             const char      *property,
                             const char      *value,
                             MediaInfo       *ctx,
                             gpointer         user_data);

static void map_to_album (TrackerResource *resource,
                          const char      *property,
                          const char      *value,
                          MediaInfo       *ctx,
                          gpointer         user_data);

static void map_to_hash (TrackerResource *resource,
                         const char      *property,
                         const char      *value,
                         MediaInfo       *ctx,
                         gpointer         user_data);

static void map_to_mb (TrackerResource *resource,
		       const char      *property,
		       const char      *value,
		       MediaInfo       *ctx,
		       gpointer         user_data);

enum {
	MB_TAG_NONE,
	MB_TAG_ALBUM,
	MB_TAG_RELEASEGROUP,
	MB_TAG_TRACK,
	MB_TAG_RELEASETRACK,
	MB_TAG_ARTIST,
	N_MB_TAGS,
};

static const char *mb_uri_prefixes[] = {
	NULL,
	"https://musicbrainz.org/release",
	"https://musicbrainz.org/release-group",
	"https://musicbrainz.org/recording",
	"https://musicbrainz.org/track",
	"https://musicbrainz.org/artist",
};

G_STATIC_ASSERT (G_N_ELEMENTS (mb_uri_prefixes) == N_MB_TAGS);

static const char *mb_uri_reference_ids[] = {
	NULL,
	"https://musicbrainz.org/doc/Release",
	"https://musicbrainz.org/doc/Release_Group",
	"https://musicbrainz.org/doc/Recording",
	"https://musicbrainz.org/doc/Track",
	"https://musicbrainz.org/doc/Artist",
};

G_STATIC_ASSERT (G_N_ELEMENTS (mb_uri_reference_ids) == N_MB_TAGS);

typedef struct
{
	const char *rdf_property;
	stream_t stream;
	const wchar_t *medialib_property;
	MappingFunc mapping_func;
	gpointer mapping_func_data;
} PropertyMapping;

static PropertyMapping mb_artist_mapping[] = {
	{ "tracker:hasExternalReference", Stream_General, L"MUSICBRAINZ_ARTISTID", map_to_mb, GUINT_TO_POINTER (MB_TAG_ARTIST) },
};

static PropertyMapping album_mapping[] = {
	{ "nmm:albumTrackCount", Stream_General, L"Track/Position_Total", map_to_int },
	{ "tracker:hasExternalReference", Stream_General, L"MUSICBRAINZ_ALBUMID", map_to_mb, GUINT_TO_POINTER (MB_TAG_ALBUM) },
	{ "tracker:hasExternalReference", Stream_General, L"MUSICBRAINZ_RELEASEGROUPID", map_to_mb, GUINT_TO_POINTER (MB_TAG_RELEASEGROUP) },
};

static PropertyMapping video_mapping[] = {
	{ "nfo:width", Stream_Video, L"Width", map_to_int },
	{ "nfo:height", Stream_Video, L"Height", map_to_int },
	{ "nfo:frameRate", Stream_Video, L"FrameRate", map_to_double },
	{ "nfo:aspectRatio", Stream_Video, L"PixelAspectRatio", map_to_double },
	{ "nfo:frameCount", Stream_Video, L"FrameCount", map_to_int },
	{ "nmm:episodeNumber", Stream_General, L"Track", map_to_int },
	{ "nmm:season", Stream_General, L"Season", map_to_int },
	{ "nmm:leadActor", Stream_General, L"ARTIST", map_to_artist },
	{ "nmm:leadActor", Stream_General, L"Artist", map_to_artist },
	{ "nmm:director", Stream_General, L"DIRECTOR", map_to_artist },
	{ "nmm:director", Stream_General, L"Director", map_to_artist },
	{ "nmm:synopsis", Stream_General, L"SYNOPSIS" },
	{ "nmm:synopsis", Stream_General, L"Synopsis" },
};

static PropertyMapping audio_common_mapping[] = {
	{ "nmm:trackNumber", Stream_General, L"Track/Position", map_to_int },
	{ "nmm:performer", Stream_General, L"Performer", map_to_artist, GUINT_TO_POINTER (TRUE) },
	{ "nie:contentCreated", Stream_General, L"Recorded_Date", map_to_datetime },
	{ "tracker:hasExternalReference", Stream_General, L"MUSICBRAINZ_TRACKID", map_to_mb, GUINT_TO_POINTER (MB_TAG_TRACK) },
	{ "tracker:hasExternalReference", Stream_General, L"MUSICBRAINZ_RELEASETRACKID", map_to_mb, GUINT_TO_POINTER (MB_TAG_RELEASETRACK) },
	{ "nmm:musicAlbum", Stream_General, L"Album", map_to_album },
	{ "nfo:hasHash", Stream_General, L"ACOUSTID_FINGERPRINT", map_to_hash, (gpointer) "chromaprint" },
};

static PropertyMapping audio_ogg_mapping[] = {
	{ "nmm:composer", Stream_General, L"Composer", map_to_composer_ogg },
};

static PropertyMapping audio_other_mapping[] = {
	{ "nmm:composer", Stream_General, L"Composer", map_to_composer },
};

static PropertyMapping common_mapping[] = {
	{ "nfo:sampleRate", Stream_Audio, L"SamplingRate", map_to_int },
	{ "nfo:channels", Stream_General, L"Audio_Channels_Total", map_to_int },
	{ "nfo:averageBitrate", Stream_General, L"OverallBitRate", map_to_int },
	{ "nfo:duration", Stream_General, L"Duration", map_msec_to_sec },
	{ "nie:contentCreated", Stream_General, L"Encoded_Date", map_to_datetime },
	{ "nie:comment", Stream_General, L"Comment" },
	{ "nie:copyright", Stream_General, L"Copyright" },
	{ "nie:description", Stream_General, L"Description" },
	{ "nfo:genre", Stream_General, L"GENRE" },
	{ "nfo:genre", Stream_General, L"Genre" },
	{ "nie:generator", Stream_General, L"Encoded_Library" },
	{ "nie:title", Stream_General, L"Title" },
};

static std::wstring
utf8_to_ucs4 (const char  *c_str,
              GError     **error)
{
	size_t required_size = mbstowcs (nullptr, c_str, 0) + 1;
	std::wstring wstr (required_size, L'\0');
	mbstowcs (&wstr[0], c_str, required_size);

	return wstr;
}

static char *
ucs4_to_utf8 (std::wstring   str,
              GError       **error)
{
	GError *inner_error = NULL;
	g_autofree char *res = NULL;

	res = g_ucs4_to_utf8 ((gunichar *) str.c_str(), str.size(), NULL, NULL, &inner_error);
	if (inner_error) {
		g_propagate_error (error, inner_error);
		return NULL;
	}

	return g_steal_pointer (&res);
}

static char *
ctx_get_property (MediaInfo    *ctx,
                  stream_t      stream,
                  std::wstring  prop)
{
	std::wstring value;

	value = ctx->Get(stream, 0, prop, Info_Text);
	if (value.size () == 0)
		return NULL;

	return ucs4_to_utf8 (value, NULL);
}

static void
add_hash (TrackerResource *resource,
          MediaInfo       *ctx,
          const char      *hash_value,
          const char      *algorithm)
{
	g_autoptr (TrackerResource) hash = NULL;
	TrackerResource *file_resource;

	file_resource = tracker_resource_get_first_relation (resource, "nie:isStoredAs");
	g_assert (file_resource);

	hash = tracker_resource_new (NULL);
	tracker_resource_set_uri (hash, "rdf:type", "nfo:FileHash");
	tracker_resource_set_string (hash, "nfo:hashValue", hash_value);
	tracker_resource_set_string (hash, "nfo:hashAlgorithm", algorithm);

	tracker_resource_add_relation (file_resource, "nfo:hasHash", hash);
}

static void
extract_gibest_hash (TrackerResource *resource,
                     MediaInfo       *ctx,
                     GFile           *file)
{
	guint64 buffer[2][CHUNK_N_BYTES/8];
	g_autoptr (GInputStream) stream = NULL;
	g_autoptr (GError) error = NULL;
	g_autofree char *hash_str = NULL;
	gssize n_bytes, file_size;
	guint64 hash = 0;
	guint i;

	stream = G_INPUT_STREAM (g_file_read (file, NULL, NULL));
	if (stream == NULL)
		return;

	/* Extract start/end chunks of the file */
	n_bytes = g_input_stream_read (stream, buffer[0], CHUNK_N_BYTES, NULL, NULL);
	if (n_bytes == -1)
		return;

	if (!g_seekable_seek (G_SEEKABLE (stream), -CHUNK_N_BYTES, G_SEEK_END, NULL, NULL)) {
		/* Files may be smaller than the chunk size */
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT))
			return;

		g_clear_error (&error);
		if (!g_seekable_seek (G_SEEKABLE (stream), 0, G_SEEK_SET, NULL, NULL))
			return;
	}

	n_bytes = g_input_stream_read (stream, buffer[1], CHUNK_N_BYTES, NULL, NULL);
	if (n_bytes == -1)
		return;

	for (i = 0; i < G_N_ELEMENTS (buffer[0]); i++)
		hash += buffer[0][i] + buffer[1][i];

	file_size = g_seekable_tell (G_SEEKABLE (stream));

	if (file_size < CHUNK_N_BYTES)
		return;

	/* Include file size */
	hash += file_size;

	hash_str = g_strdup_printf ("%" G_GINT64_MODIFIER "x", hash);
	add_hash (resource, ctx, hash_str, "gibest");
}

static void
apply_metadata (MediaInfo       *ctx,
                TrackerResource *resource,
                PropertyMapping *properties,
                int              n_properties)
{
	int i;

	for (i = 0; i < n_properties; i++) {
		PropertyMapping *prop = &properties[i];
		g_autofree char *value = NULL;

		value = ctx_get_property (ctx, prop->stream, prop->medialib_property);
		if (!value)
			continue;

		if (prop->mapping_func)
			prop->mapping_func (resource, prop->rdf_property, value, ctx, prop->mapping_func_data);
		else
			tracker_resource_set_string (resource, prop->rdf_property, value);
	}
}

static void
map_to_int (TrackerResource *resource,
            const char      *property,
            const char      *value,
            MediaInfo       *ctx,
            gpointer         user_data)
{
	int64_t value_int;

	if (sscanf (value, "%" G_GINT64_MODIFIER "u", &value_int) == 1)
		tracker_resource_set_int64 (resource, property, value_int);
}

static void
map_to_double (TrackerResource *resource,
               const char      *property,
               const char      *value,
               MediaInfo       *ctx,
               gpointer         user_data)
{
	double value_double;

	value_double = g_ascii_strtod (value, NULL);
	if (errno == 0)
		tracker_resource_set_double (resource, property, value_double);
}

static void
map_to_datetime (TrackerResource *resource,
                 const char      *property,
                 const char      *value,
                 MediaInfo       *ctx,
                 gpointer         user_data)
{
	g_autofree char *date = NULL;

	date = tracker_date_format_to_iso8601 (value, "%Y-%m-%d %H:%M:%S %Z");
	if (!date)
		date = tracker_date_format_to_iso8601 (value, "%Y%Z");
	if (date)
		tracker_resource_set_string (resource, property, date);
}

static void
map_msec_to_sec (TrackerResource *resource,
                 const char      *property,
                 const char      *value,
                 MediaInfo       *ctx,
                 gpointer         user_data)
{
	int64_t value_int;

	if (sscanf (value, "%" G_GINT64_MODIFIER "u", &value_int) == 1)
		tracker_resource_set_int64 (resource, property, value_int / 1000);
}

static void
map_to_artist (TrackerResource *resource,
               const char      *property,
               const char      *value,
               MediaInfo       *ctx,
               gpointer         user_data)
{
	gboolean apply_mb_data = GPOINTER_TO_UINT (user_data);
	g_auto (GStrv) strv = NULL;
	int i;

	strv = g_strsplit (value, ",", -1);

	for (i = 0; strv[i]; i++) {
		g_autoptr (TrackerResource) child =NULL;
		TrackerResource *artist;

		child = tracker_extract_new_artist (g_strstrip (strv[i]));

		if (apply_mb_data)
			apply_metadata (ctx, child, mb_artist_mapping, G_N_ELEMENTS (mb_artist_mapping));

		if (i == 0)
			tracker_resource_set_relation (resource, property, child);
		else
			tracker_resource_add_relation (resource, property, child);

		artist = tracker_resource_get_first_relation (resource, "nmm:artist");
		if (!artist)
			tracker_resource_set_relation (resource, "nmm:artist", child);
	}
}

static void
map_to_composer_ogg (TrackerResource *resource,
                     const char      *property,
                     const char      *value,
                     MediaInfo       *ctx,
                     gpointer         user_data)
{
	g_auto (GStrv) strv = NULL;
	int i;

	strv = g_strsplit (value, "/", -1);

	for (i = 0; strv[i]; i++) {
		g_autoptr (TrackerResource) child =NULL;

		child = tracker_extract_new_artist (g_strstrip (strv[i]));
		apply_metadata (ctx, child, mb_artist_mapping, G_N_ELEMENTS (mb_artist_mapping));

		if (i == 0)
			tracker_resource_set_relation (resource, property, child);
		else
			tracker_resource_set_relation (resource, "nmm:artist", child);
	}
}

static void
map_to_composer (TrackerResource *resource,
                 const char      *property,
                 const char      *value,
                 MediaInfo       *ctx,
                 gpointer         user_data)
{
	g_auto (GStrv) strv = NULL;
	int i;

	strv = g_strsplit (value, "/", -1);

	for (i = 0; strv[i]; i++) {
		g_autoptr (TrackerResource) child =NULL;

		child = tracker_extract_new_artist (g_strstrip (strv[i]));
		apply_metadata (ctx, child, mb_artist_mapping, G_N_ELEMENTS (mb_artist_mapping));

		if (i == 0)
			tracker_resource_set_relation (resource, property, child);

		tracker_resource_set_relation (resource, "nmm:artist", child);
	}
}

static void
map_to_album (TrackerResource *resource,
              const char      *property,
              const char      *value,
              MediaInfo       *ctx,
              gpointer         user_data)
{
	g_autofree char *disc_nr_str = NULL, *album_artist_str = NULL;
	g_autoptr (TrackerResource) album_disc = NULL, artist = NULL;
	const char *content_created;
	TrackerResource *album;
	gint64 disc_number;

	disc_nr_str = ctx_get_property (ctx, Stream_General, L"Part");
	if (!disc_nr_str || sscanf (disc_nr_str, "%" G_GINT64_MODIFIER "u", &disc_number) != 1)
		disc_number = 1;

	album_artist_str = ctx_get_property (ctx, Stream_General, L"Album/Performer");
	if (!album_artist_str)
		album_artist_str = ctx_get_property (ctx, Stream_General, L"Album/Composer");
	if (album_artist_str)
		artist = tracker_extract_new_artist (album_artist_str);

	content_created = tracker_resource_get_first_string (resource, "nie:contentCreated");

	album_disc = tracker_extract_new_music_album_disc (value,
	                                                   artist,
	                                                   disc_number,
	                                                   content_created);

	album = tracker_resource_get_first_relation (album_disc, "nmm:albumDiscAlbum");

	apply_metadata (ctx, album, album_mapping, G_N_ELEMENTS (album_mapping));

	tracker_resource_set_relation (resource, property, album);
	tracker_resource_set_relation (resource, "nmm:musicAlbumDisc", album_disc);
}

static void
map_to_hash (TrackerResource *resource,
             const char      *property,
             const char      *value,
             MediaInfo       *ctx,
             gpointer         user_data)
{
	const char *hash_algorithm = (const char *) user_data;

	add_hash (resource, ctx, value, hash_algorithm);
}

static void
map_to_mb (TrackerResource *resource,
           const char      *property,
           const char      *value,
           MediaInfo       *ctx,
           gpointer         user_data)
{
	guint type = GPOINTER_TO_UINT (user_data);
	g_autoptr (TrackerResource) reference = NULL;
	g_autofree char *uri = NULL;

	g_assert (type > 0 && type < N_MB_TAGS);

	uri = g_strdup_printf ("%s/%s", mb_uri_prefixes[type], value);
	reference = tracker_extract_new_external_reference (mb_uri_reference_ids[type],
	                                                    value, uri);
	tracker_resource_add_relation (resource, property, reference);
}

static void
apply_cuesheet (TrackerExtractInfo *info,
                TrackerResource    *metadata,
                MediaInfo          *ctx)
{
	g_autofree char *cuesheet_str = NULL, *uri = NULL;
	TrackerToc *cue_sheet = NULL;

	cuesheet_str = ctx_get_property (ctx, Stream_General, L"CUESHEET");

	if (cuesheet_str) {
		g_auto (GStrv) lines = NULL;
		g_autofree char *cuesheet_formatted = NULL;

		lines = g_strsplit (cuesheet_str, "/", -1);
		cuesheet_formatted = g_strjoinv ("\n", lines);

		cue_sheet = tracker_cue_sheet_parse (cuesheet_formatted);
	} else {
		GFile *file;

		file = tracker_extract_info_get_file (info);
		uri = g_file_get_uri (file);

		cue_sheet = tracker_cue_sheet_guess_from_uri (uri);
	}

	if (cue_sheet) {
		tracker_cue_sheet_apply_to_resource (cue_sheet,
		                                     metadata,
		                                     info);
		tracker_toc_free (cue_sheet);
	}
}

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info,
                              GError             **error)
{
	g_autofree char *path = NULL, *uri = NULL, *res_utf8 = NULL;
	g_autoptr (TrackerResource) metadata = NULL, file_resource = NULL;
	const char *resource_uri;
	std::wstring path_str;
	GFile *file;
	gboolean has_video, has_audio, success = FALSE;
	MediaInfo *ctx = NULL;
#ifdef HAVE_EXEMPI
	TrackerXmpData *xmp = NULL;
#endif
#ifdef HAVE_GUPNP_DLNA
	g_autoptr (GUPnPDLNAInformation) gupnp_info = NULL;
	g_autoptr (GUPnPDLNAProfileGuesser) profile_guesser = NULL;
	GUPnPDLNAProfile *gupnp_profile = NULL;
#endif

	resource_uri = tracker_extract_info_get_content_id (info, NULL);
	metadata = tracker_resource_new (resource_uri);

	file = tracker_extract_info_get_file (info);
	path = g_file_get_path (file);
	uri = g_file_get_uri (file);

	path_str = utf8_to_ucs4 (path, NULL);

	if (path_str.size () == 0)
		return FALSE;

	ctx = new MediaInfo ();

	ctx->Option (L"Internet", L"no");

	if (ctx->Open (String (path_str)) <= 0) {
		g_set_error (error,
		             G_IO_ERROR,
		             G_IO_ERROR_FAILED,
		             "Could not open %s", uri);
		goto out;
	}

	has_video = ctx->Count_Get (Stream_Video, -1);
	has_audio = ctx->Count_Get (Stream_Audio, -1);

	file_resource = tracker_resource_new (uri);
	tracker_resource_set_relation (metadata, "nie:isStoredAs", file_resource);

	if (has_video || has_audio)
		apply_metadata (ctx, metadata, common_mapping, G_N_ELEMENTS (common_mapping));

	if (has_video) {
		tracker_resource_add_uri (metadata, "rdf:type", "nmm:Video");

		apply_metadata (ctx, metadata, video_mapping, G_N_ELEMENTS (video_mapping));

		extract_gibest_hash (metadata, ctx, file);
	} else if (has_audio) {
		g_autofree char *format = NULL;

		tracker_resource_add_uri (metadata, "rdf:type", "nmm:MusicPiece");
		tracker_resource_add_uri (metadata, "rdf:type", "nfo:Audio");

		apply_metadata (ctx, metadata, audio_common_mapping, G_N_ELEMENTS (audio_common_mapping));

		format = ctx_get_property (ctx, Stream_General, L"Format");

		if (g_strcmp0 (format, "Ogg") == 0)
			apply_metadata (ctx, metadata, audio_ogg_mapping, G_N_ELEMENTS (audio_ogg_mapping));
		else
			apply_metadata (ctx, metadata, audio_other_mapping, G_N_ELEMENTS (audio_other_mapping));

		apply_cuesheet (info, metadata, ctx);
	} else {
		goto out;
	}

#ifdef HAVE_EXEMPI
	xmp = tracker_xmp_new_for_file (file);

	if (xmp) {
		tracker_xmp_apply_to_resource (metadata, xmp);
		tracker_xmp_free (xmp);
	}
#endif

#ifdef HAVE_GUPNP_DLNA
	gupnp_info = tracker_gupnp_dlna_information_new (ctx);
	profile_guesser = gupnp_dlna_profile_guesser_new (FALSE, FALSE);

	gupnp_profile = gupnp_dlna_profile_guesser_guess_profile_from_info (profile_guesser,
	                                                                    gupnp_info);

	if (gupnp_profile) {
		const char *profile_name, *profile_mime;

		profile_mime = gupnp_dlna_profile_get_mime (gupnp_profile);
		if (profile_mime)
			tracker_resource_set_string (metadata, "nmm:dlnaMime", profile_mime);

		profile_name = gupnp_dlna_profile_get_name (gupnp_profile);
		if (profile_name)
			tracker_resource_set_string (metadata, "nmm:dlnaProfile", profile_name);
	}
#endif

	tracker_extract_info_set_resource (info, metadata);
	success = TRUE;

 out:
	ctx->Close ();

	delete ctx;

	return success;
}
