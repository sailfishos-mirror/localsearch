/*
 * Copyright (C) 2026 Red Hat Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 * Author: Carlos Garnacho <carlosg@gnome.org>
 */

#include "config-miners.h"

#include "tracker-gupnp-mediainfo.h"

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
ctx_get_string_property (MediaInfo    *ctx,
                         stream_t      stream,
                         std::wstring  prop)
{
	std::wstring value;

	value = ctx->Get(stream, 0, prop, Info_Text);
	if (value.size () == 0)
		return NULL;

	return ucs4_to_utf8 (value, NULL);
}

static gboolean
ctx_get_int_property (MediaInfo    *ctx,
                      stream_t      stream,
                      std::wstring  prop,
                      int          *value_out)
{
	g_autofree char *value = NULL;

	value = ctx_get_string_property (ctx, stream, prop);

	if (value && sscanf (value, "%u", value_out) == 1)
		return TRUE;

	return FALSE;
}

typedef struct _TrackerGUPnPDLNAAudioInformation TrackerGUPnPDLNAAudioInformation;

struct _TrackerGUPnPDLNAAudioInformation {
	GUPnPDLNAAudioInformation parent;
	MediaInfo *ctx;
};

#define TRACKER_TYPE_GUPNP_DLNA_AUDIO_INFORMATION (tracker_gupnp_dlna_audio_information_get_type ())
G_DECLARE_FINAL_TYPE (TrackerGUPnPDLNAAudioInformation,
                      tracker_gupnp_dlna_audio_information,
                      TRACKER, GUPNP_DLNA_AUDIO_INFORMATION,
                      GUPnPDLNAAudioInformation)

G_DEFINE_TYPE (TrackerGUPnPDLNAAudioInformation,
               tracker_gupnp_dlna_audio_information,
               GUPNP_TYPE_DLNA_AUDIO_INFORMATION)

#define DLNA_INT(v) ((GUPnPDLNAIntValue) { .value = v, .state = GUPNP_DLNA_VALUE_STATE_SET })
#define DLNA_BOOL(v) ((GUPnPDLNABoolValue) { .value = v, .state = GUPNP_DLNA_VALUE_STATE_SET })
#define DLNA_STRING(v) ((GUPnPDLNAStringValue) { .value = g_strdup((v)), .state = GUPNP_DLNA_VALUE_STATE_SET })
#define DLNA_FRAC(n, d) ((GUPnPDLNAFractionValue) { .numerator = n, .denominator = d, .state = GUPNP_DLNA_VALUE_STATE_SET })

static GUPnPDLNAIntValue
tracker_gupnp_dlna_audio_information_get_bitrate (GUPnPDLNAAudioInformation *info)
{
	TrackerGUPnPDLNAAudioInformation *audio_info =
		TRACKER_GUPNP_DLNA_AUDIO_INFORMATION (info);
	int bitrate;

	if (!ctx_get_int_property (audio_info->ctx, Stream_Audio, L"BitRate", &bitrate))
		return GUPNP_DLNA_INT_VALUE_UNSET;

	return DLNA_INT (bitrate);
}

static GUPnPDLNAIntValue
tracker_gupnp_dlna_audio_information_get_channels (GUPnPDLNAAudioInformation *info)
{
	TrackerGUPnPDLNAAudioInformation *audio_info =
		TRACKER_GUPNP_DLNA_AUDIO_INFORMATION (info);
	int n_channels;

	if (!ctx_get_int_property (audio_info->ctx, Stream_Audio, L"Channel(s)", &n_channels))
		return GUPNP_DLNA_INT_VALUE_UNSET;

	return DLNA_INT (n_channels);
}

static GUPnPDLNAIntValue
tracker_gupnp_dlna_audio_information_get_depth (GUPnPDLNAAudioInformation *info)
{
	TrackerGUPnPDLNAAudioInformation *audio_info =
		TRACKER_GUPNP_DLNA_AUDIO_INFORMATION (info);
	int bit_depth;

	if (!ctx_get_int_property (audio_info->ctx, Stream_Audio, L"BitDepth", &bit_depth))
		return GUPNP_DLNA_INT_VALUE_UNSET;

	return GUPNP_DLNA_INT_VALUE_UNSET;
}

static GUPnPDLNAIntValue
tracker_gupnp_dlna_audio_information_get_layer (GUPnPDLNAAudioInformation *info)
{
	TrackerGUPnPDLNAAudioInformation *audio_info =
		TRACKER_GUPNP_DLNA_AUDIO_INFORMATION (info);
	g_autofree char *format = NULL, *format_profile = NULL;
	int layer;

	format = ctx_get_string_property (audio_info->ctx, Stream_Audio, L"Format_Commercial");
	if (g_strcmp0 (format, "MPEG Audio") != 0)
		return GUPNP_DLNA_INT_VALUE_UNSET;

	format_profile = ctx_get_string_property (audio_info->ctx, Stream_Audio, L"Format_Profile");

	if (format_profile && sscanf (format_profile, "Layer %d", &layer) == 1)
		return DLNA_INT (layer);

	return GUPNP_DLNA_INT_VALUE_UNSET;
}

static GUPnPDLNAStringValue
tracker_gupnp_dlna_audio_information_get_level (GUPnPDLNAAudioInformation *info)
{
	TrackerGUPnPDLNAAudioInformation *audio_info =
		TRACKER_GUPNP_DLNA_AUDIO_INFORMATION (info);
	g_autofree char *format = NULL, *format_level = NULL;
	int sample_rate, n_channels;

	format_level = ctx_get_string_property (audio_info->ctx, Stream_Audio, L"Format_Level");
	if (format_level)
		return DLNA_STRING (format_level);

	format = ctx_get_string_property (audio_info->ctx, Stream_Audio, L"Format_Commercial");

	if (g_strcmp0 (format, "AAC") == 0 &&
	    ctx_get_int_property (audio_info->ctx, Stream_Audio, L"SamplingRate", &sample_rate) &&
	    ctx_get_int_property (audio_info->ctx, Stream_Audio, L"Channel(s)", &n_channels)) {
		const char *value = NULL;

		if (n_channels <= 2 && sample_rate <= 24000)
			value = "1";
		else if (n_channels <= 2 && sample_rate <= 48000)
			value = "2";
		else if (n_channels <= 5 && sample_rate <= 48000)
			value = "4";
		else if (n_channels <= 5 && sample_rate <= 96000)
			value = "5";
		else if (n_channels <= 7 && sample_rate <= 48000)
			value = "6";
		else if (n_channels <= 7 && sample_rate <= 96000)
			value = "7";

		if (value)
			return DLNA_STRING (value);
	}

	return GUPNP_DLNA_STRING_VALUE_UNSET;
}

static GUPnPDLNAIntValue
tracker_gupnp_dlna_audio_information_get_mpeg_audio_version (GUPnPDLNAAudioInformation *info)
{
	return GUPNP_DLNA_INT_VALUE_UNSET;
}

static GUPnPDLNAIntValue
tracker_gupnp_dlna_audio_information_get_mpeg_version (GUPnPDLNAAudioInformation *info)
{
	TrackerGUPnPDLNAAudioInformation *audio_info =
		TRACKER_GUPNP_DLNA_AUDIO_INFORMATION (info);
	g_autofree char *format = NULL, *format_version = NULL;
	int version;

	format = ctx_get_string_property (audio_info->ctx, Stream_Audio, L"Format_Commercial");

	if (g_strcmp0 (format, "AAC") == 0) {
		return DLNA_INT (4);
	} else if (g_strcmp0 (format, "MPEG Audio") == 0) {
		format_version = ctx_get_string_property (audio_info->ctx, Stream_Audio, L"Format_Version");

		if (format_version && sscanf (format_version, "Version %d", &version) == 1)
			return DLNA_INT (version);
	}

	return GUPNP_DLNA_INT_VALUE_UNSET;
}

static GUPnPDLNAStringValue
tracker_gupnp_dlna_audio_information_get_profile (GUPnPDLNAAudioInformation *info)
{
	TrackerGUPnPDLNAAudioInformation *audio_info =
		TRACKER_GUPNP_DLNA_AUDIO_INFORMATION (info);
	g_autofree char *format = NULL, *features = NULL, *lower = NULL;

	format = ctx_get_string_property (audio_info->ctx, Stream_Audio, L"Format_Commercial");

	if (g_strcmp0 (format, "AAC") == 0) {
		features = ctx_get_string_property (audio_info->ctx, Stream_Audio, L"Format_AdditionalFeatures");
		if (features) {
			lower = g_utf8_strdown (features, -1);
			return DLNA_STRING (lower);
		}
	}

	return GUPNP_DLNA_STRING_VALUE_UNSET;
}

static GUPnPDLNAIntValue
tracker_gupnp_dlna_audio_information_get_rate (GUPnPDLNAAudioInformation *info)
{
	TrackerGUPnPDLNAAudioInformation *audio_info =
		TRACKER_GUPNP_DLNA_AUDIO_INFORMATION (info);
	int sampling_rate;

	if (!ctx_get_int_property (audio_info->ctx, Stream_Audio, L"SamplingRate", &sampling_rate))
		return GUPNP_DLNA_INT_VALUE_UNSET;

	return DLNA_INT (sampling_rate);
}

static GUPnPDLNAStringValue
tracker_gupnp_dlna_audio_information_get_stream_format (GUPnPDLNAAudioInformation *info)
{
	TrackerGUPnPDLNAAudioInformation *audio_info =
		TRACKER_GUPNP_DLNA_AUDIO_INFORMATION (info);
	g_autofree char *format = NULL;

	format = ctx_get_string_property (audio_info->ctx, Stream_Audio, L"Format");

	if (g_strcmp0 (format, "AAC") == 0)
		return DLNA_STRING ("raw");
	else if (g_strcmp0 (format, "ADTS") == 0)
		return DLNA_STRING ("adts");

	return GUPNP_DLNA_STRING_VALUE_UNSET;
}

static GUPnPDLNAIntValue
tracker_gupnp_dlna_audio_information_get_wma_version (GUPnPDLNAAudioInformation *info)
{
	TrackerGUPnPDLNAAudioInformation *audio_info =
		TRACKER_GUPNP_DLNA_AUDIO_INFORMATION (info);
	g_autofree char *format = NULL, *format_version = NULL;
	int version;

	format = ctx_get_string_property (audio_info->ctx, Stream_Audio, L"Format_Commercial");

	if (g_strcmp0 (format, "WMA") == 0) {
		format_version = ctx_get_string_property (audio_info->ctx, Stream_Audio, L"Format_Version");

		if (format_version && sscanf (format_version, "Version %d", &version) == 1)
			return DLNA_INT (version);
	}

	return GUPNP_DLNA_INT_VALUE_UNSET;
}

static GUPnPDLNAStringValue
tracker_gupnp_dlna_audio_information_get_mime (GUPnPDLNAAudioInformation *info)
{
	TrackerGUPnPDLNAAudioInformation *audio_info =
		TRACKER_GUPNP_DLNA_AUDIO_INFORMATION (info);
	g_autofree char *format = NULL, *mimetype = NULL;

	format = ctx_get_string_property (audio_info->ctx, Stream_Audio, L"Format");
	if (g_strcmp0 (format, "AAC") == 0)
		return DLNA_STRING ("audio/mpeg");
	else if (g_strcmp0 (format, "WMA") == 0)
		return DLNA_STRING ("audio/x-wma");

	mimetype = ctx_get_string_property (audio_info->ctx, Stream_Audio, L"InternetMediaType");

	if (!mimetype)
		mimetype = ctx_get_string_property (audio_info->ctx, Stream_General, L"InternetMediaType");

	return DLNA_STRING (mimetype ? mimetype : "");
}

static void
tracker_gupnp_dlna_audio_information_class_init (TrackerGUPnPDLNAAudioInformationClass *klass)
{
	GUPnPDLNAAudioInformationClass *audio_info_class =
		GUPNP_DLNA_AUDIO_INFORMATION_CLASS (klass);

	audio_info_class->get_bitrate =
		tracker_gupnp_dlna_audio_information_get_bitrate;
	audio_info_class->get_channels =
		tracker_gupnp_dlna_audio_information_get_channels;
	audio_info_class->get_depth =
		tracker_gupnp_dlna_audio_information_get_depth;
	audio_info_class->get_layer =
		tracker_gupnp_dlna_audio_information_get_layer;
	audio_info_class->get_level =
		tracker_gupnp_dlna_audio_information_get_level;
	audio_info_class->get_mpeg_audio_version =
		tracker_gupnp_dlna_audio_information_get_mpeg_audio_version;
	audio_info_class->get_mpeg_version =
		tracker_gupnp_dlna_audio_information_get_mpeg_version;
	audio_info_class->get_profile =
		tracker_gupnp_dlna_audio_information_get_profile;
	audio_info_class->get_rate =
		tracker_gupnp_dlna_audio_information_get_rate;
	audio_info_class->get_stream_format =
		tracker_gupnp_dlna_audio_information_get_stream_format;
	audio_info_class->get_wma_version =
		tracker_gupnp_dlna_audio_information_get_wma_version;
	audio_info_class->get_mime =
		tracker_gupnp_dlna_audio_information_get_mime;
}

static void
tracker_gupnp_dlna_audio_information_init (TrackerGUPnPDLNAAudioInformation *info)
{
}

typedef struct _TrackerGUPnPDLNAVideoInformation TrackerGUPnPDLNAVideoInformation;
struct _TrackerGUPnPDLNAVideoInformation {
	GUPnPDLNAVideoInformation parent;
	MediaInfo *ctx;
};

#define TRACKER_TYPE_GUPNP_DLNA_VIDEO_INFORMATION (tracker_gupnp_dlna_video_information_get_type ())
G_DECLARE_FINAL_TYPE (TrackerGUPnPDLNAVideoInformation,
                      tracker_gupnp_dlna_video_information,
                      TRACKER, GUPNP_DLNA_VIDEO_INFORMATION,
                      GUPnPDLNAVideoInformation)

G_DEFINE_TYPE (TrackerGUPnPDLNAVideoInformation,
               tracker_gupnp_dlna_video_information,
               GUPNP_TYPE_DLNA_VIDEO_INFORMATION)

static GUPnPDLNAIntValue
tracker_gupnp_dlna_video_information_get_bitrate (GUPnPDLNAVideoInformation *info)
{
	TrackerGUPnPDLNAVideoInformation *video_info =
		TRACKER_GUPNP_DLNA_VIDEO_INFORMATION (info);
	int bitrate;

	if (!ctx_get_int_property (video_info->ctx, Stream_Video, L"BitRate", &bitrate))
		return GUPNP_DLNA_INT_VALUE_UNSET;

	return DLNA_INT (bitrate);
}

static GUPnPDLNAFractionValue
tracker_gupnp_dlna_video_information_get_framerate (GUPnPDLNAVideoInformation *info)
{
	TrackerGUPnPDLNAVideoInformation *video_info =
		TRACKER_GUPNP_DLNA_VIDEO_INFORMATION (info);
	int num, den;

	if (!ctx_get_int_property (video_info->ctx, Stream_Video, L"FrameRate_Num", &num))
		return GUPNP_DLNA_FRACTION_VALUE_UNSET;
	if (!ctx_get_int_property (video_info->ctx, Stream_Video, L"FrameRate_Den", &den))
		return GUPNP_DLNA_FRACTION_VALUE_UNSET;

	return DLNA_FRAC (num, den);
}

static GUPnPDLNAIntValue
tracker_gupnp_dlna_video_information_get_height (GUPnPDLNAVideoInformation *info)
{
	TrackerGUPnPDLNAVideoInformation *video_info =
		TRACKER_GUPNP_DLNA_VIDEO_INFORMATION (info);
	int height;

	if (!ctx_get_int_property (video_info->ctx, Stream_Video, L"Height", &height))
		return GUPNP_DLNA_INT_VALUE_UNSET;

	return DLNA_INT (height);
}

static GUPnPDLNABoolValue
tracker_gupnp_dlna_video_information_is_interlaced (GUPnPDLNAVideoInformation *info)
{
	TrackerGUPnPDLNAVideoInformation *video_info =
		TRACKER_GUPNP_DLNA_VIDEO_INFORMATION (info);
	g_autofree char *scantype = NULL;

	scantype = ctx_get_string_property (video_info->ctx, Stream_Video, L"ScanType");

	return DLNA_BOOL (g_strcmp0 (scantype, "Interlaced") == 0);
}

static GUPnPDLNAStringValue
tracker_gupnp_dlna_video_information_get_level (GUPnPDLNAVideoInformation *info)
{
	TrackerGUPnPDLNAVideoInformation *video_info =
		TRACKER_GUPNP_DLNA_VIDEO_INFORMATION (info);
	g_autofree char *format_level = NULL;

	format_level = ctx_get_string_property (video_info->ctx, Stream_Video, L"Format_Level");
	if (format_level)
		DLNA_STRING (format_level);

	return GUPNP_DLNA_STRING_VALUE_UNSET;
}

static GUPnPDLNAIntValue
tracker_gupnp_dlna_video_information_get_mpeg_version (GUPnPDLNAVideoInformation *info)
{
	TrackerGUPnPDLNAVideoInformation *video_info =
		TRACKER_GUPNP_DLNA_VIDEO_INFORMATION (info);
	g_autofree char *format = NULL, *format_version = NULL;
	int version;

	format = ctx_get_string_property (video_info->ctx, Stream_Video, L"Format_Commercial");

	if (g_strcmp0 (format, "MPEG Video") == 0) {
		format_version = ctx_get_string_property (video_info->ctx, Stream_Video, L"Format_Version");

		if (format_version && sscanf (format_version, "Version %d", &version) == 1)
			return DLNA_INT (version);
	}

	return GUPNP_DLNA_INT_VALUE_UNSET;
}

static GUPnPDLNAFractionValue
tracker_gupnp_dlna_video_information_get_pixel_aspect_ratio (GUPnPDLNAVideoInformation *info)
{
	TrackerGUPnPDLNAVideoInformation *video_info =
		TRACKER_GUPNP_DLNA_VIDEO_INFORMATION (info);
	g_autofree char *display_aspect_ratio = NULL;
	int den, num, width, height;

	display_aspect_ratio = ctx_get_string_property (video_info->ctx, Stream_Video, L"DisplayAspectRatio/String");

	if (!display_aspect_ratio || sscanf (display_aspect_ratio, "%d:%d", &num, &den) != 2)
		return GUPNP_DLNA_FRACTION_VALUE_UNSET;
	if (!ctx_get_int_property (video_info->ctx, Stream_Video, L"Width", &width))
		return GUPNP_DLNA_FRACTION_VALUE_UNSET;
	if (!ctx_get_int_property (video_info->ctx, Stream_Video, L"Height", &height))
		return GUPNP_DLNA_FRACTION_VALUE_UNSET;

	return DLNA_FRAC (height * num, width * den);
}

static GUPnPDLNAStringValue
tracker_gupnp_dlna_video_information_get_profile (GUPnPDLNAVideoInformation *info)
{
	TrackerGUPnPDLNAVideoInformation *video_info =
		TRACKER_GUPNP_DLNA_VIDEO_INFORMATION (info);
	g_autofree char *format_profile;

	format_profile = ctx_get_string_property (video_info->ctx, Stream_Video, L"Format_Profile");
	if (format_profile)
		return DLNA_STRING (format_profile);

	return GUPNP_DLNA_STRING_VALUE_UNSET;
}

static GUPnPDLNABoolValue
tracker_gupnp_dlna_video_information_is_system_stream (GUPnPDLNAVideoInformation *info)
{
	return GUPNP_DLNA_BOOL_VALUE_UNSET;
}

static GUPnPDLNAIntValue
tracker_gupnp_dlna_video_information_get_width (GUPnPDLNAVideoInformation *info)
{
	TrackerGUPnPDLNAVideoInformation *video_info =
		TRACKER_GUPNP_DLNA_VIDEO_INFORMATION (info);
	int width;

	if (!ctx_get_int_property (video_info->ctx, Stream_Video, L"Width", &width))
		return GUPNP_DLNA_INT_VALUE_UNSET;

	return DLNA_INT (width);
}

static GUPnPDLNAStringValue
tracker_gupnp_dlna_video_information_get_mime (GUPnPDLNAVideoInformation *info)
{
	TrackerGUPnPDLNAVideoInformation *video_info =
		TRACKER_GUPNP_DLNA_VIDEO_INFORMATION (info);
	g_autofree char *mimetype;

	mimetype = ctx_get_string_property (video_info->ctx, Stream_Video, L"InternetMediaType");

	if (!mimetype)
		mimetype = ctx_get_string_property (video_info->ctx, Stream_General, L"InternetMediaType");

	return DLNA_STRING (mimetype ? mimetype : "");
}

static void
tracker_gupnp_dlna_video_information_class_init (TrackerGUPnPDLNAVideoInformationClass *klass)
{
	GUPnPDLNAVideoInformationClass *video_info_class =
		GUPNP_DLNA_VIDEO_INFORMATION_CLASS (klass);

	video_info_class->get_bitrate =
		tracker_gupnp_dlna_video_information_get_bitrate;
	video_info_class->get_framerate =
		tracker_gupnp_dlna_video_information_get_framerate;
	video_info_class->get_height =
		tracker_gupnp_dlna_video_information_get_height;
	video_info_class->is_interlaced =
		tracker_gupnp_dlna_video_information_is_interlaced;
	video_info_class->get_level =
		tracker_gupnp_dlna_video_information_get_level;
	video_info_class->get_mpeg_version =
		tracker_gupnp_dlna_video_information_get_mpeg_version;
	video_info_class->get_pixel_aspect_ratio =
		tracker_gupnp_dlna_video_information_get_pixel_aspect_ratio;
	video_info_class->get_profile =
		tracker_gupnp_dlna_video_information_get_profile;
	video_info_class->is_system_stream =
		tracker_gupnp_dlna_video_information_is_system_stream;
	video_info_class->get_width =
		tracker_gupnp_dlna_video_information_get_width;
	video_info_class->get_mime =
		tracker_gupnp_dlna_video_information_get_mime;
}

static void
tracker_gupnp_dlna_video_information_init (TrackerGUPnPDLNAVideoInformation *info)
{
}

typedef struct _TrackerGUPnPDLNAContainerInformation TrackerGUPnPDLNAContainerInformation;
struct _TrackerGUPnPDLNAContainerInformation {
	GUPnPDLNAContainerInformation parent;
	MediaInfo *ctx;
};

#define TRACKER_TYPE_GUPNP_DLNA_CONTAINER_INFORMATION (tracker_gupnp_dlna_container_information_get_type ())
G_DECLARE_FINAL_TYPE (TrackerGUPnPDLNAContainerInformation,
                      tracker_gupnp_dlna_container_information,
                      TRACKER, GUPNP_DLNA_CONTAINER_INFORMATION,
                      GUPnPDLNAContainerInformation)

G_DEFINE_TYPE (TrackerGUPnPDLNAContainerInformation,
               tracker_gupnp_dlna_container_information,
               GUPNP_TYPE_DLNA_CONTAINER_INFORMATION)

static GUPnPDLNAIntValue
tracker_gupnp_dlna_container_information_get_mpeg_version (GUPnPDLNAContainerInformation *info)
{
	TrackerGUPnPDLNAContainerInformation *cont_info =
		TRACKER_GUPNP_DLNA_CONTAINER_INFORMATION (info);
	g_autofree char *format = NULL;

	format = ctx_get_string_property (cont_info->ctx, Stream_General, L"Format_Commercial");

	if (g_strcmp0 (format, "AAC") == 0)
		return DLNA_INT (4);

	return GUPNP_DLNA_INT_VALUE_UNSET;
}

static GUPnPDLNAIntValue
tracker_gupnp_dlna_container_information_get_packet_size (GUPnPDLNAContainerInformation *info)
{
	return GUPNP_DLNA_INT_VALUE_UNSET;
}

static GUPnPDLNAStringValue
tracker_gupnp_dlna_container_information_get_profile (GUPnPDLNAContainerInformation *info)
{
	return GUPNP_DLNA_STRING_VALUE_UNSET;
}

static GUPnPDLNABoolValue
tracker_gupnp_dlna_container_information_is_system_stream (GUPnPDLNAContainerInformation *info)
{
	TrackerGUPnPDLNAContainerInformation *cont_info =
		TRACKER_GUPNP_DLNA_CONTAINER_INFORMATION (info);
	g_autofree char *format = NULL;
	const char *system_stream_formats[] = {
		"MPEG-PS",
		"MPEG-TS",
		"RealMedia",
		"DV",
		NULL,
	};

	format = ctx_get_string_property (cont_info->ctx, Stream_General, L"Format");

	if (g_strv_contains (system_stream_formats, format))
		return DLNA_BOOL (TRUE);

	return GUPNP_DLNA_BOOL_VALUE_UNSET;
}

static GUPnPDLNAStringValue
tracker_gupnp_dlna_container_information_get_variant (GUPnPDLNAContainerInformation *info)
{
	TrackerGUPnPDLNAContainerInformation *cont_info =
		TRACKER_GUPNP_DLNA_CONTAINER_INFORMATION (info);
	g_autofree char *mimetype = NULL;
	const char *iso_mimetypes[] = {
		"audio/mp4",
		"video/mp4",
	};

	mimetype = ctx_get_string_property (cont_info->ctx, Stream_General, L"InternetMediaType");

	if (mimetype && g_strv_contains (iso_mimetypes, mimetype))
		return DLNA_STRING ("iso");

	return GUPNP_DLNA_STRING_VALUE_UNSET;
}

static GUPnPDLNAStringValue
tracker_gupnp_dlna_container_information_get_mime (GUPnPDLNAContainerInformation *info)
{
	TrackerGUPnPDLNAContainerInformation *cont_info =
		TRACKER_GUPNP_DLNA_CONTAINER_INFORMATION (info);
	g_autofree char *mimetype;
	const char *quicktime_mimetypes[] = {
		"audio/mp4",
		"video/mp4",
		"audio/3gpp",
		"video/3gpp",
		"audio/3gpp2",
		"video/3gpp2",
		NULL,
	};
	const char *asf_mimetypes[] = {
		"audio/x-ms-wma",
		"audio/x-wma",
	};

	mimetype = ctx_get_string_property (cont_info->ctx, Stream_General, L"InternetMediaType");

	if (mimetype && g_strv_contains (quicktime_mimetypes, mimetype))
		return DLNA_STRING ("video/quicktime");
	else if (mimetype && g_strv_contains (asf_mimetypes, mimetype))
		return DLNA_STRING ("video/x-ms-asf");

	return DLNA_STRING (mimetype ? mimetype : "");
}

static void
tracker_gupnp_dlna_container_information_class_init (TrackerGUPnPDLNAContainerInformationClass *klass)
{
	GUPnPDLNAContainerInformationClass *cont_info_class =
		GUPNP_DLNA_CONTAINER_INFORMATION_CLASS (klass);

	cont_info_class->get_mpeg_version =
		tracker_gupnp_dlna_container_information_get_mpeg_version;
	cont_info_class->get_packet_size =
		tracker_gupnp_dlna_container_information_get_packet_size;
	cont_info_class->get_profile =
		tracker_gupnp_dlna_container_information_get_profile;
	cont_info_class->is_system_stream =
		tracker_gupnp_dlna_container_information_is_system_stream;
	cont_info_class->get_variant =
		tracker_gupnp_dlna_container_information_get_variant;
	cont_info_class->get_mime =
		tracker_gupnp_dlna_container_information_get_mime;
}

static void
tracker_gupnp_dlna_container_information_init (TrackerGUPnPDLNAContainerInformation *info)
{
}

typedef struct _TrackerGUPnPDLNAInformation TrackerGUPnPDLNAInformation;
struct _TrackerGUPnPDLNAInformation {
	GUPnPDLNAInformation parent;
	MediaInfo *ctx;
};

#define TRACKER_TYPE_GUPNP_DLNA_INFORMATION (tracker_gupnp_dlna_information_get_type ())
G_DECLARE_FINAL_TYPE (TrackerGUPnPDLNAInformation,
                      tracker_gupnp_dlna_information,
                      TRACKER, GUPNP_DLNA_INFORMATION,
                      GUPnPDLNAInformation)

G_DEFINE_TYPE (TrackerGUPnPDLNAInformation,
               tracker_gupnp_dlna_information,
               GUPNP_TYPE_DLNA_INFORMATION)

static GUPnPDLNAAudioInformation *
tracker_gupnp_dlna_information_get_audio_information (GUPnPDLNAInformation *info)
{
	TrackerGUPnPDLNAInformation *tracker_info =
		TRACKER_GUPNP_DLNA_INFORMATION (info);
	GUPnPDLNAAudioInformation *audio = NULL;

	if (tracker_info->ctx->Count_Get (Stream_Audio, -1) > 0) {
		TrackerGUPnPDLNAAudioInformation *audio_info;

		audio_info = (TrackerGUPnPDLNAAudioInformation *) g_object_new (TRACKER_TYPE_GUPNP_DLNA_AUDIO_INFORMATION, NULL);
		audio_info->ctx = tracker_info->ctx;
		audio = GUPNP_DLNA_AUDIO_INFORMATION (audio_info);
	}

	return audio;
}

static GUPnPDLNAVideoInformation *
tracker_gupnp_dlna_information_get_video_information (GUPnPDLNAInformation *info)
{
	TrackerGUPnPDLNAInformation *tracker_info =
		TRACKER_GUPNP_DLNA_INFORMATION (info);
	GUPnPDLNAVideoInformation *video = NULL;

	if (tracker_info->ctx->Count_Get (Stream_Video, -1) > 0) {
		TrackerGUPnPDLNAVideoInformation *video_info;

		video_info = (TrackerGUPnPDLNAVideoInformation *) g_object_new (TRACKER_TYPE_GUPNP_DLNA_VIDEO_INFORMATION, NULL);
		video_info->ctx = tracker_info->ctx;
		video = GUPNP_DLNA_VIDEO_INFORMATION (video_info);
	}

	return video;
}

static GUPnPDLNAContainerInformation *
tracker_gupnp_dlna_information_get_container_information (GUPnPDLNAInformation *info)
{
	TrackerGUPnPDLNAInformation *tracker_info =
		TRACKER_GUPNP_DLNA_INFORMATION (info);
	GUPnPDLNAContainerInformation *container = NULL;

	if (tracker_info->ctx->Count_Get (Stream_Audio, -1) > 0 ||
	    tracker_info->ctx->Count_Get (Stream_Video, -1) > 0) {
		TrackerGUPnPDLNAContainerInformation *container_info;

		container_info = (TrackerGUPnPDLNAContainerInformation *) g_object_new (TRACKER_TYPE_GUPNP_DLNA_CONTAINER_INFORMATION, NULL);
		container_info->ctx = tracker_info->ctx;
		container = GUPNP_DLNA_CONTAINER_INFORMATION (container_info);
	}

	return container;
}

static GUPnPDLNAImageInformation *
tracker_gupnp_dlna_information_get_image_information (GUPnPDLNAInformation *info)
{
	return NULL;
}

static void
tracker_gupnp_dlna_information_class_init (TrackerGUPnPDLNAInformationClass *klass)
{
	GUPnPDLNAInformationClass *dlna_info_class =
		GUPNP_DLNA_INFORMATION_CLASS (klass);

	dlna_info_class->get_audio_information =
		tracker_gupnp_dlna_information_get_audio_information;
	dlna_info_class->get_video_information =
		tracker_gupnp_dlna_information_get_video_information;
	dlna_info_class->get_container_information =
		tracker_gupnp_dlna_information_get_container_information;
	dlna_info_class->get_image_information =
		tracker_gupnp_dlna_information_get_image_information;
}

static void
tracker_gupnp_dlna_information_init (TrackerGUPnPDLNAInformation *info)
{
}

GUPnPDLNAInformation *
tracker_gupnp_dlna_information_new (MediaInfo *ctx)
{
	TrackerGUPnPDLNAInformation *info;

	info = (TrackerGUPnPDLNAInformation *) g_object_new (TRACKER_TYPE_GUPNP_DLNA_INFORMATION, NULL);
	info->ctx = ctx;

	return GUPNP_DLNA_INFORMATION (info);
}
