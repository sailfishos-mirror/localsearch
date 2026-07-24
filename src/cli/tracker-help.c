/*
 * Copyright (C) 2014, Lanedo <martyn@lanedo.com>
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

#include <unistd.h>
#include <errno.h>

#include <glib/gi18n.h>

#include "tracker-help.h"

static int
exec_man (const char *path,
	  const char *page)
{
	const gchar *argv[] = {
		path,
		page,
		NULL
	};
	gboolean retval;
	int status;

	g_return_val_if_fail (path != NULL, -1);
	g_return_val_if_fail (page != NULL, -1);

	retval = g_spawn_sync (NULL, (gchar**) argv, NULL,
	                       G_SPAWN_SEARCH_PATH,
	                       NULL, NULL, NULL, NULL,
	                       &status, NULL);

	return (!retval || !g_spawn_check_wait_status (status, NULL)) ? -1 : 0;
}

int
tracker_help_show_man_page (const char *cmd)
{
	g_autofree char *page = NULL;

	g_return_val_if_fail (cmd != NULL, -1);

	page = g_strconcat (MAIN_COMMAND_NAME "-", cmd, NULL);

	return exec_man ("man", page);
}

int
tracker_help (int          argc,
	      const char **argv)
{
	return tracker_help_show_man_page (argv[1]);
}
