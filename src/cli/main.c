/*
 * Copyright (C) 2014, Lanedo <martyn@lanedo.com>
 * Copyright (C) 2024, Sam Thursfield <sam@afuera.me.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "config-miners.h"

#include <stdlib.h>
#include <locale.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#include <tracker-common.h>

#include "tracker-daemon.h"
#include "tracker-extract.h"
#include "tracker-index.h"
#include "tracker-info.h"
#include "tracker-inhibit.h"
#include "tracker-reset.h"
#include "tracker-search.h"
#include "tracker-status.h"
#include "tracker-tag.h"

const char usage_string[] =
	"localsearch [--version] [--help]\n"
	"            <command> [<args>]";

const char about[] =
	"LocalSearch " PACKAGE_VERSION "\n"
	"\n"
	"This program is free software and comes without any warranty.\n"
	"It is licensed under version 2 or later of the General Public "
	"License which can be viewed at:\n"
	"\n"
	"  http://www.gnu.org/licenses/gpl.txt"
	"\n";

struct cmd_struct {
	const char *cmd;
	int (*fn)(int, const char **);
	const char *help;
};

static int launch_external_command (int argc, const char **argv);

static void print_usage (void);

static struct cmd_struct commands[] = {
	{ "daemon", tracker_daemon, N_("Start and stop the indexer") },
	{ "extract", tracker_extract, N_("Extract metadata from a file") },
	{ "index", tracker_index, N_("List and change indexed folders") },
	{ "info", tracker_info, N_("Retrieve information available for files and resources") },
	{ "inhibit", tracker_inhibit, N_("Inhibit indexing temporarily") },
	{ "reset", tracker_reset, N_("Erase the indexed data") },
	{ "search", tracker_search, N_("Search for content") },
	{ "status", tracker_status, N_("Provide status and statistics on the data indexed") },
	{ "tag", tracker_tag, N_("Add, remove and list tags") },
	{ "test-sandbox", launch_external_command, N_("Sandbox for a testing environment") },
};

static int
launch_external_command (int          argc,
                         const char **argv)
{
	const char *execdir, *subcommand;
	char *path, *basename;

	execdir = g_getenv ("LOCALSEARCH_CLI_PATH");
	if (!execdir)
		execdir = PYTHON_UTILS_DIR;

	/* Execute subcommand binary */
	subcommand = argv[0];
	basename = g_strdup_printf("localsearch3-%s", subcommand);
	path = g_build_filename (execdir, basename, NULL);

	/* Manipulate argv in place, in order to launch subcommand */
	argv[0] = path;
	return execv (path, (char * const *) argv);
}

static int
print_version (void)
{
	puts (about);
	return 0;
}

static inline void
mput_char (char c, unsigned int num)
{
      while (num--) {
              putchar (c);
      }
}

static void
print_usage_list_cmds (void)
{
        guint longest = 0;
        guint i;

        puts (_("Available localsearch commands are:"));

        for (i = 0; i < G_N_ELEMENTS (commands); i++) {
                if (longest < strlen (commands[i].cmd))
                        longest = strlen (commands[i].cmd);
        }

        for (i = 0; i < G_N_ELEMENTS (commands); i++) {
                g_print ("   %s   ", commands[i].cmd);
                mput_char (' ', longest - strlen (commands[i].cmd));
                puts (_(commands[i].help));
        }

#if 0
	guint longest = 0;
	GList *commands = NULL;
	GList *c;
	GFileEnumerator *enumerator;
	GFileInfo *info;
	GFile *dir;
	GError *error = NULL;
	const gchar *cli_metadata_dir;

	cli_metadata_dir = g_getenv ("TRACKER_CLI_DIR");

	if (!cli_metadata_dir) {
		cli_metadata_dir = CLI_METADATA_DIR;
	}

	dir = g_file_new_for_path (cli_metadata_dir);
	enumerator = g_file_enumerate_children (dir,
	                                        G_FILE_ATTRIBUTE_STANDARD_NAME,
	                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
	                                        NULL, &error);
	g_object_unref (dir);

	if (enumerator) {
		while ((info = g_file_enumerator_next_file (enumerator, NULL, NULL)) != NULL) {
			const gchar *filename;

			filename = g_file_info_get_name (info);
			if (g_str_has_suffix (filename, ".desktop")) {
				gchar *path = NULL;
				GDesktopAppInfo *desktop_info;

				path = g_build_filename (cli_metadata_dir, filename, NULL);
				desktop_info = g_desktop_app_info_new_from_filename (path);
				if (desktop_info) {
					commands = g_list_prepend (commands, desktop_info);
				} else {
					g_warning ("Unable to load command info: %s", path);
				}

				g_free (path);
			}
			g_object_unref (info);
		}

		g_object_unref (enumerator);
	} else {
		g_warning ("Failed to list commands: %s", error->message);
	}

	puts (_("Available localsearch commands are:"));

	if (commands) {
		commands = g_list_sort (commands, (GCompareFunc) compare_app_info);

		for (c = commands; c; c = c->next) {
			GDesktopAppInfo *desktop_info = c->data;
			const gchar *name = g_app_info_get_name (G_APP_INFO (desktop_info));

			if (longest < strlen (name))
				longest = strlen (name);
		}

		for (c = commands; c; c = c->next) {
			GDesktopAppInfo *desktop_info = c->data;
			const gchar *name = g_app_info_get_name (G_APP_INFO (desktop_info));
			const gchar *help = g_app_info_get_description (G_APP_INFO (desktop_info));

			g_print ("   %s   ", name);
			mput_char (' ', longest - strlen (name));
			puts (help);
		}

		g_list_free_full (commands, g_object_unref);
	}
#endif
}

static void
print_usage (void)
{
	g_print ("usage: %s\n\n", usage_string);
	print_usage_list_cmds ();
	g_print ("\n%s\n", _("See “localsearch help <command>” to read about a specific subcommand."));
}

int
main (int argc, char *argv[])
{
        int (* func) (int, const char *[]) = NULL;
        const gchar *subcommand = argv[1];
	int i;

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	if (argc == 1) {
		/* The user didn't specify a command; give them help */
		print_usage ();
		exit (EXIT_SUCCESS);
	}

	if (g_strcmp0 (subcommand, "--version") == 0) {
		print_version ();
		exit (EXIT_SUCCESS);
	} else if (g_strcmp0 (subcommand, "--help") == 0) {
		subcommand = "help";
	}

	if (g_strcmp0 (subcommand, "help") == 0 && argc == 2) {
		/* Print usage here to avoid duplicating it in tracker-help.c */
		print_usage ();
		exit (EXIT_SUCCESS);
	}

	for (i = 0; i < G_N_ELEMENTS (commands); i++) {
		if (g_strcmp0 (commands[i].cmd, subcommand) == 0)
			func = commands[i].fn;
	}

        if (func) {
                return func (argc - 1, (const char **) &argv[1]);
        } else {
                g_printerr (_("“%s” is not a localsearch command. See “localsearch --help”"), subcommand);
                g_printerr ("\n");
                return EXIT_FAILURE;
        }

	return EXIT_FAILURE;
}
