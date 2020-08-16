/*
 * Copyright (C) 2020, Red Hat Inc.
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
 *
 * Author: Carlos Garnacho <carlosg@gnome.org>
 */

#include "tracker-term-utils.h"

#include <sys/ioctl.h>
#include <unistd.h>

static guint n_columns = 0;
static guint n_rows = 0;

gchar *
tracker_term_ellipsize (const gchar          *str,
                        gint                  max_len,
                        TrackerEllipsizeMode  mode)
{
	gint len = strlen (str);
	gchar *substr, *retval;

	if (len < max_len)
		return g_strdup (str);

	if (mode == TRACKER_ELLIPSIZE_START) {
		substr = g_memdup (str + len - max_len + 1, max_len - 1);
		retval = g_strdup_printf ("…%s", substr);
		g_free (substr);
	} else {
		substr = g_memdup (str, max_len - 1);
		retval = g_strdup_printf ("%s…", substr);
		g_free (substr);
	}

	return retval;
}

static gboolean
fd_term_dimensions (gint  fd,
                    gint *cols,
                    gint *rows)
{
        struct winsize ws = {};

        if (ioctl(fd, TIOCGWINSZ, &ws) < 0)
                return FALSE;

        if (ws.ws_col <= 0 || ws.ws_row <= 0)
                return FALSE;

        *cols = ws.ws_col;
        *rows = ws.ws_row;

        return TRUE;
}

void
tracker_term_dimensions (guint *columns,
                         guint *rows)
{
	if (n_columns == 0 || n_rows == 0)
		fd_term_dimensions (STDOUT_FILENO, &n_columns, &n_rows);

	if (n_columns <= 0)
		n_columns = 80;
	if (n_rows <= 0)
		n_rows = 24;

	if (columns)
		*columns = n_columns;
	if (rows)
		*rows = n_rows;
}
