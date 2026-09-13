/*
 * Copyright (C) 2019, Sam Thursfield <sam@afuera.me.uk>
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

#include <glib.h>

#include <tracker-common.h>

#define assert_path_basename(path, cmp, expected_value) {    \
	g_autofree gchar *basename = g_path_get_basename (path); \
	g_assert_cmpstr (basename, cmp, expected_value);         \
}

static gchar *
get_test_rules_dir (void)
{
	return g_build_path (G_DIR_SEPARATOR_S, TOP_SRCDIR, "tests", "common", "test-extract-rules", NULL);
}

static void
init_rules_manager (void) {
	gboolean success;
	g_autofree gchar *test_rules_dir = NULL;

	test_rules_dir = get_test_rules_dir ();
	success = g_setenv ("TRACKER_EXTRACTOR_RULES_DIR", test_rules_dir, TRUE);
	g_assert_true (success);
}

static void
test_extract_rules (void)
{
	g_autoptr (TrackerExtractRulesManager) manager = NULL;
	GError *error = NULL;
	GStrv strv;

	manager = tracker_extract_rules_manager_new (&error);
	g_assert_no_error (error);

	/* The audio rule should match this, but the image rule should not. */
	strv = tracker_extract_rules_manager_get_rdf_types (manager, "audio/mpeg");
	g_assert_cmpint (g_strv_length (strv), ==, 1);
	g_assert_cmpstr (strv[0], ==, "nfo:Audio");
	g_assert_nonnull (tracker_extract_rules_manager_get_module (manager, "audio/mpeg"));
	g_clear_pointer (&strv, g_strfreev);

	/* The image rule should match this, but the audio rule should not. */
	strv = tracker_extract_rules_manager_get_rdf_types (manager, "image/png");
	g_assert_cmpint (g_strv_length (strv), ==, 2);
	g_assert_true (g_strv_contains ((const char * const *) strv, "nfo:Image"));
	g_assert_nonnull (tracker_extract_rules_manager_get_module (manager, "image/png"));
	g_clear_pointer (&strv, g_strfreev);

	/* No rule should match this. */
	strv = tracker_extract_rules_manager_get_rdf_types (manager, "text/generic");
	g_assert_cmpint (g_strv_length (strv), ==, 0);
	g_assert_null (tracker_extract_rules_manager_get_graph (manager, "text/generic"));
	g_assert_null (tracker_extract_rules_manager_get_hash (manager, "text/generic"));
	g_assert_null (tracker_extract_rules_manager_get_module (manager, "text/generic"));
	g_clear_pointer (&strv, g_strfreev);

	/* The image/x-blocked MIME type is explicitly blocked, so no rule should match. */
	strv = tracker_extract_rules_manager_get_rdf_types (manager, "image/x-blocked");
	g_assert_cmpint (g_strv_length (strv), ==, 0);
	g_assert_null (tracker_extract_rules_manager_get_graph (manager, "image/x-blocked"));
	g_assert_null (tracker_extract_rules_manager_get_hash (manager, "image/x-blocked"));
	g_assert_null (tracker_extract_rules_manager_get_module (manager, "image/x-blocked"));
	g_clear_pointer (&strv, g_strfreev);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	init_rules_manager ();

	g_test_add_func ("/libtracker-extract/module-manager/extract-rules",
	                 test_extract_rules);
	return g_test_run ();
}
