/*
 * Copyright (C) 2006, Jamie McCracken <jamiemcc@gnome.org>
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
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
 */

#include "config-miners.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gif_lib.h>

#include <tracker-common.h>

#include "utils/tracker-extract.h"

#ifdef HAVE_EXEMPI
#include "tracker-xmp.h"
#endif

#define XMP_MAGIC_TRAILER_LENGTH 256
#define EXTENSION_RECORD_COMMENT_BLOCK_CODE 0xFE

typedef struct {
	unsigned int   byteCount;
	char          *bytes;
} ExtBlock;

static int
ext_block_append(ExtBlock *extBlock,
		 unsigned int len,
		 unsigned char extData[])
{
	extBlock->bytes = realloc(extBlock->bytes,extBlock->byteCount+len);
	if (extBlock->bytes == NULL) {
		return (GIF_ERROR);
	}

	memcpy(&(extBlock->bytes[extBlock->byteCount]), &extData[0], len);
	extBlock->byteCount += len;

	return (GIF_OK);
}

#if GIFLIB_MAJOR >= 5
static inline void
gif_error (const gchar *action, int err)
{
	const char *str = GifErrorString (err);
	if (str != NULL) {
		g_debug ("%s, error: '%s'", action, str);
	} else {
		g_debug ("%s, undefined error %d", action, err);
	}
}
#else /* GIFLIB_MAJOR >= 5 */
static inline void print_gif_error()
{
#if defined(GIFLIB_MAJOR) && defined(GIFLIB_MINOR) && ((GIFLIB_MAJOR == 4 && GIFLIB_MINOR >= 2) || GIFLIB_MAJOR > 4)
	const char *str = GifErrorString ();
	if (str != NULL) {
		g_debug ("GIF, error: '%s'", str);
	} else {
		g_debug ("GIF, undefined error");
	}
#else
	PrintGifError();
#endif
}
#endif /* GIFLIB_MAJOR >= 5 */

/* giflib 5.1 changed the API of DGifCloseFile to take two arguments */
#if !defined(GIFLIB_MAJOR) || \
    !(GIFLIB_MAJOR > 5 || (GIFLIB_MAJOR == 5 && GIFLIB_MINOR >= 1))
#define DGifCloseFile(a, b) DGifCloseFile(a)
#endif

static TrackerResource *
read_metadata (GifFileType        *gifFile,
               GFile              *file,
               const gchar        *uri,
               TrackerExtractInfo *info)
{
	TrackerResource *metadata;
	GifRecordType RecordType;
	int frameheight;
	int framewidth;
	unsigned char *framedata = NULL;
	gint h;
	int status;
#ifdef HAVE_EXEMPI
	TrackerXmpData *xd = NULL;
#endif
	gchar *sidecar = NULL;
	int width = 0, height = 0;
	g_autofree char *resource_uri = NULL, *comment = NULL;

	do {
		GifByteType *ExtData;
		int ExtCode;
		ExtBlock extBlock;

		if (DGifGetRecordType(gifFile, &RecordType) == GIF_ERROR) {
#if GIFLIB_MAJOR < 5
			print_gif_error ();
#else  /* GIFLIB_MAJOR < 5 */
			gif_error ("Could not read next GIF record type", gifFile->Error);
#endif /* GIFLIB_MAJOR < 5 */
			return NULL;
		}

		switch (RecordType) {
			case IMAGE_DESC_RECORD_TYPE:
			if (DGifGetImageDesc(gifFile) == GIF_ERROR) {
#if GIFLIB_MAJOR < 5
				print_gif_error();
#else  /* GIFLIB_MAJOR < 5 */
				gif_error ("Could not get GIF record information", gifFile->Error);
#endif /* GIFLIB_MAJOR < 5 */
				return NULL;
			}

			framewidth  = gifFile->Image.Width;
			frameheight = gifFile->Image.Height;

			framedata = g_malloc_n (framewidth, sizeof(GifPixelType));
			for (h = 0; h < frameheight; h++)
			{
				if (DGifGetLine(gifFile, framedata, framewidth)==GIF_ERROR) {
					g_free (framedata);
#if GIFLIB_MAJOR < 5
					print_gif_error();
#else  /* GIFLIB_MAJOR < 5 */
					gif_error ("Could not load a block of GIF pixes", gifFile->Error);
#endif /* GIFLIB_MAJOR < 5 */
					return NULL;
				}
			}

			width = framewidth;
			height = frameheight;
			g_free (framedata);

		break;
		case EXTENSION_RECORD_TYPE:
			extBlock.bytes = NULL;
			extBlock.byteCount = 0;

			if ((status = DGifGetExtension (gifFile, &ExtCode, &ExtData)) != GIF_OK) {
				g_warning ("Problem getting the extension");
				return NULL;
			}
#if defined(HAVE_EXEMPI)
			if (ExtData && *ExtData &&
			    strncmp (&ExtData[1],"XMP Data",8) == 0) {
				while (ExtData != NULL && status == GIF_OK ) {
					if ((status = DGifGetExtensionNext (gifFile, &ExtData)) == GIF_OK) {
						if (ExtData != NULL) {
							if (ext_block_append (&extBlock, ExtData[0]+1, (char *) &(ExtData[0])) != GIF_OK) {
								g_warning ("Problem with extension data");
								return NULL;
							}
						}
					}
				}

				if (extBlock.byteCount > XMP_MAGIC_TRAILER_LENGTH) {
					xd = tracker_xmp_new (extBlock.bytes,
							      extBlock.byteCount -
							      XMP_MAGIC_TRAILER_LENGTH,
							      uri);
				}

				g_free (extBlock.bytes);
			} else
#endif
			/* See Section 24. Comment Extension. in the GIF format definition */
			if (ExtCode == EXTENSION_RECORD_COMMENT_BLOCK_CODE &&
			    ExtData && *ExtData) {
				guint block_count = 0;

				/* Merge all blocks */
				do {
					block_count++;

					g_debug ("Comment Extension block found (#%u, %u bytes)",
					         block_count,
					         ExtData[0]);
					if (ext_block_append (&extBlock, ExtData[0], (char *) &(ExtData[1])) != GIF_OK) {
						g_warning ("Problem with Comment extension data");
						return NULL;
					}
				} while (((status = DGifGetExtensionNext(gifFile, &ExtData)) == GIF_OK) &&
				         ExtData != NULL);

				/* Add last NUL byte */
				g_debug ("Comment Extension blocks found (%u) with %u bytes",
				         block_count,
				         extBlock.byteCount);
				extBlock.bytes = g_realloc (extBlock.bytes, extBlock.byteCount + 1);
				extBlock.bytes[extBlock.byteCount] = '\0';

				/* Set comment */
				g_clear_pointer (&comment, g_free);
				comment = extBlock.bytes;
			} else {
				do {
					status = DGifGetExtensionNext(gifFile, &ExtData);
				} while ( status == GIF_OK && ExtData != NULL);
			}
		break;
		case TERMINATE_RECORD_TYPE:
			break;
		default:
			break;
		}
	} while (RecordType != TERMINATE_RECORD_TYPE);

	resource_uri = tracker_extract_info_get_content_id (info, NULL);
	metadata = tracker_resource_new (resource_uri);

#ifdef HAVE_EXEMPI
	if (!xd) {
		xd = tracker_xmp_new_from_sidecar (file, &sidecar);

		if (sidecar) {
			TrackerResource *sidecar_resource;

			sidecar_resource = tracker_resource_new (sidecar);
			tracker_resource_add_uri (sidecar_resource, "rdf:type", "nfo:FileDataObject");
			tracker_resource_set_uri (sidecar_resource, "nie:interpretedAs", resource_uri);

			tracker_resource_add_take_relation (metadata, "nie:isStoredAs", sidecar_resource);
		}
	}
#endif

	tracker_resource_add_uri (metadata, "rdf:type", "nfo:Image");
	tracker_resource_add_uri (metadata, "rdf:type", "nmm:Photo");

	tracker_guarantee_resource_date_from_file_mtime (metadata,
	                                                 "nie:contentCreated",
	                                                 NULL,
	                                                 uri);

	tracker_guarantee_resource_title_from_file (metadata,
	                                            "nie:title",
	                                            NULL,
	                                            uri,
	                                            NULL);

	if (width > 0)
		tracker_resource_set_int (metadata, "nfo:width", width);

	if (height > 0)
		tracker_resource_set_int (metadata, "nfo:height", height);

	if (comment)
		tracker_guarantee_resource_utf8_string (metadata, "nie:comment", comment);

#ifdef HAVE_EXEMPI
	if (xd) {
		tracker_xmp_apply_to_resource (metadata, xd);
		tracker_xmp_free (xd);
	}
#endif

	return metadata;
}


G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info,
                              GError             **error)
{
	TrackerResource *metadata;
	goffset size;
	GifFileType *gifFile = NULL;
	gchar *filename, *uri;
	GFile *file;
	int fd;
#if GIFLIB_MAJOR >= 5
	int err;
#endif

	file = tracker_extract_info_get_file (info);
	filename = g_file_get_path (file);
	size = tracker_file_get_size (filename);

	if (size < 64) {
		g_free (filename);
		return FALSE;
	}

	fd = tracker_file_open_fd (filename);

	if (fd == -1) {
		g_set_error (error,
		             G_IO_ERROR,
		             g_io_error_from_errno (errno),
		             "Could not open GIF file: %s\n",
		             g_strerror (errno));
		g_free (filename);
		return FALSE;
	}

#if GIFLIB_MAJOR < 5
	if ((gifFile = DGifOpenFileHandle (fd)) == NULL) {
		print_gif_error ();
#else   /* GIFLIB_MAJOR < 5 */
	if ((gifFile = DGifOpenFileHandle (fd, &err)) == NULL) {
		gif_error ("Could not open GIF file with handle", err);
#endif /* GIFLIB_MAJOR < 5 */
		g_free (filename);
		close (fd);
		return FALSE;
	}

	g_free (filename);

	uri = g_file_get_uri (file);

	metadata = read_metadata (gifFile, file, uri, info);

	g_free (uri);

	if (DGifCloseFile (gifFile, NULL) != GIF_OK) {
#if GIFLIB_MAJOR < 5
		print_gif_error ();
#else  /* GIFLIB_MAJOR < 5 */
		gif_error ("Could not close GIF file", gifFile->Error);
#endif /* GIFLIB_MAJOR < 5 */
	}

	if (metadata) {
		tracker_extract_info_set_resource (info, metadata);
		g_object_unref (metadata);
	}

	close (fd);

	return TRUE;
}
