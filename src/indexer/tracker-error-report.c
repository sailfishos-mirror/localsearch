/*
 * Copyright (C) 2020, Red Hat Ltd
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

#include "tracker-error-report.h"

#include <glib/gstdio.h>
#include <errno.h>

#include "tracker-common.h"
#include "tracker-utils.h"

#define GROUP "Report"
#define KEY_URI "Uri"
#define KEY_MESSAGE "Message"
#define KEY_SPARQL "Sparql"

typedef struct _FileReport FileReport;
struct _FileReport {
	GFile *file;
	GKeyFile *keyfile;
};

struct _TrackerErrorReport {
	GObject parent_instance;
	GFile *report_dir;
	GThreadPool *error_thread;
};

static void tracker_error_report_initable_iface_init (GInitableIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (TrackerErrorReport, tracker_error_report, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                      tracker_error_report_initable_iface_init))

static FileReport *
file_report_new (GFile    *file,
                 GKeyFile *keyfile)
{
	FileReport *report;

	report = g_new0 (FileReport, 1);
	report->file = g_object_ref (file);
	report->keyfile = keyfile ? g_key_file_ref (keyfile) : NULL;

	return report;
}

static void
file_report_free (FileReport *report)
{
	g_clear_object (&report->file);
	g_clear_pointer (&report->keyfile, g_key_file_unref);
	g_free (report);
}

static gchar *
get_report_file (TrackerErrorReport *errors,
                 GFile              *file)
{
	g_autofree char *uri = NULL;
	g_autoptr (GFile) report_file = NULL;
	gchar *md5;

	uri = g_file_get_uri (file);
	md5 = g_compute_checksum_for_string (G_CHECKSUM_MD5, uri, -1);
	report_file = g_file_get_child (errors->report_dir, md5);
	g_free (md5);

	return g_file_get_path (report_file);
}

static void
error_thread_func (gpointer data,
                   gpointer user_data)
{
	TrackerErrorReport *errors = user_data;
	FileReport *report = data;
	g_autoptr (GError) error = NULL;
	g_autofree gchar *report_path = NULL, *content = NULL;
	gssize len;

	report_path = get_report_file (errors, report->file);

	if (report->keyfile) {
		if (g_mkdir_with_parents (g_file_peek_path (errors->report_dir), 0700) < 0) {
			g_warning_once ("Failed to create location for error reports: %m");
			file_report_free (report);
			return;
		}

		content = g_key_file_to_data (report->keyfile, &len, NULL);

#if GLIB_CHECK_VERSION (2, 66, 0)
		if (!g_file_set_contents_full (report_path, content, len,
		                               G_FILE_SET_CONTENTS_CONSISTENT |
		                               G_FILE_SET_CONTENTS_DURABLE,
		                               0666, &error))
#else
		if (!g_file_set_contents (report_path, data, len, &error))
#endif
		{
			g_warning ("Could not save error report: %s\n", error->message);
		}
	} else if (g_remove (report_path) < 0) {
		if (errno != ENOENT) {
			g_warning ("Error removing path '%s': %m",
			           report_path);
		}
	}

	file_report_free (report);
}

static void
tracker_error_report_finalize (GObject *object)
{
	TrackerErrorReport *errors = TRACKER_ERROR_REPORT (object);

	g_clear_object (&errors->report_dir);
	g_thread_pool_free (errors->error_thread, TRUE, TRUE);

	G_OBJECT_CLASS (tracker_error_report_parent_class)->finalize (object);
}

static void
tracker_error_report_class_init (TrackerErrorReportClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_error_report_finalize;
}

static void
tracker_error_report_init (TrackerErrorReport *errors)
{
	g_autofree char *cache_dir = NULL, *errors_dir = NULL;

	cache_dir = tracker_get_cache_dir ();
	errors_dir = g_build_filename (cache_dir, "errors", NULL);
	errors->report_dir = g_file_new_for_path (errors_dir);
}

static gboolean
tracker_error_report_initable_init (GInitable     *initable,
                                    GCancellable  *cancellable,
                                    GError       **error)
{
	TrackerErrorReport *errors = TRACKER_ERROR_REPORT (initable);

	errors->error_thread = g_thread_pool_new_full (error_thread_func,
	                                               errors,
	                                               (GDestroyNotify) file_report_free,
	                                               1, FALSE,
	                                               error);
	return errors->error_thread != NULL;
}

static void
tracker_error_report_initable_iface_init (GInitableIface *iface)
{
	iface->init = tracker_error_report_initable_init;
}

TrackerErrorReport *
tracker_error_report_new (GError **error)
{
	return g_initable_new (TRACKER_TYPE_ERROR_REPORT, NULL, error, NULL);
}

void
tracker_error_report_save (TrackerErrorReport *errors,
                           GFile              *file,
                           const gchar        *error_message,
                           const gchar        *sparql)
{
	g_autoptr (GKeyFile) key_file = NULL;
	g_autofree gchar *uri = NULL;
	g_autoptr (GError) error = NULL;
	FileReport *report;

	uri = g_file_get_uri (file);
	key_file = g_key_file_new ();
	g_key_file_set_string (key_file, GROUP, KEY_URI, uri);

	if (error_message)
		g_key_file_set_string (key_file, GROUP, KEY_MESSAGE, error_message);

	if (sparql)
		g_key_file_set_string (key_file, GROUP, KEY_SPARQL, sparql);

	report = file_report_new (file, key_file);
	g_thread_pool_push (errors->error_thread, report, &error);

	if (error)
		g_warning ("Could not push task to error thread: %s", error->message);
}

void
tracker_error_report_delete (TrackerErrorReport *errors,
                             GFile              *file)
{
	g_autofree char *report_path = NULL, *uri = NULL;
	g_autoptr (GError) error = NULL;
	FileReport *report;

	report_path = get_report_file (errors, file);
	if (!g_file_test (report_path, G_FILE_TEST_EXISTS))
		return;

	report = file_report_new (file, NULL);
	g_thread_pool_push (errors->error_thread, report, &error);

	if (error)
		g_warning ("Could not push delete task to error thread: %s", error->message);
}
