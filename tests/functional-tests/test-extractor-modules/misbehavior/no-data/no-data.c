#include <libtracker-extract/tracker-extract.h>

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info,
                              GError             **error)
{
	/* Return TRUE without metadata */
	return TRUE;
}
