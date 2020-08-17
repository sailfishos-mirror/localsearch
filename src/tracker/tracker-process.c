/*
 * Copyright (C) 2010, Nokia <ivan.frade@nokia.com>
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

#include <stdlib.h>
#include <errno.h>

#include <glib.h>
#include <glib/gi18n.h>

#ifdef __OpenBSD__
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <kvm.h>
#include <unistd.h>
#endif

#ifdef __sun
#define _STRUCTURED_PROC 1
#include <sys/procfs.h>
#endif

#include "tracker-process.h"

static TrackerProcessData *
process_data_new (gchar *cmd, pid_t pid)
{
	TrackerProcessData *pd;

	pd = g_slice_new0 (TrackerProcessData);
	pd->cmd = cmd;
	pd->pid = pid;

	return pd;
}

void
tracker_process_data_free (TrackerProcessData *pd)
{
	if (!pd) {
		return;
	}

	g_free (pd->cmd);
	g_slice_free (TrackerProcessData, pd);
}

static gchar *
find_command (pid_t pid)
{
	gchar *proc_path, path[PATH_MAX];

	proc_path = g_strdup_printf ("/proc/%d/exe", pid);
	readlink (proc_path, path, PATH_MAX);

	return g_path_get_basename (path);
}

static pid_t
get_pid_for_service (GDBusConnection *connection,
                     const gchar     *name)
{
	GDBusMessage *message, *reply;
	GVariant *variant;
	guint32 process_id;

	message = g_dbus_message_new_method_call ("org.freedesktop.DBus",
	                                          "/org/freedesktop/DBus",
	                                          "org.freedesktop.DBus",
	                                          "GetConnectionUnixProcessID");
	g_dbus_message_set_body (message,
	                         g_variant_new ("(s)", name));
	reply = g_dbus_connection_send_message_with_reply_sync (connection,
	                                                        message,
	                                                        G_DBUS_SEND_MESSAGE_FLAGS_NONE,
	                                                        -1,
	                                                        NULL,
	                                                        NULL,
	                                                        NULL);
	g_object_unref (message);

	if (!reply)
		return -1;

	if (g_dbus_message_get_error_name (reply)) {
		g_object_unref (reply);
		return -1;
	}

	variant = g_dbus_message_get_body (reply);
	g_variant_get (variant, "(u)", &process_id);
	g_object_unref (reply);

	return (pid_t) process_id;
}

GSList *
tracker_process_find_all (void)
{
	GDBusConnection *connection;
	pid_t miner_fs, miner_rss;
	GSList *processes = NULL;
	TrackerProcessData *data;

	connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
	miner_fs = get_pid_for_service (connection, "org.freedesktop.Tracker3.Miner.Files");
	if (miner_fs > 0) {
		data = process_data_new (find_command (miner_fs), miner_fs);
		processes = g_slist_prepend (processes, data);
	}

	miner_rss = get_pid_for_service (connection, "org.freedesktop.Tracker3.Miner.RSS");
	if (miner_rss > 0) {
		data = process_data_new (find_command (miner_rss), miner_rss);
		processes = g_slist_prepend (processes, data);
	}

	g_object_unref (connection);

	return processes;
}

gint
tracker_process_stop (TrackerProcessTypes daemons_to_term,
                      TrackerProcessTypes daemons_to_kill)
{
	GSList *pids, *l;
	gchar *str;

	if (daemons_to_kill == TRACKER_PROCESS_TYPE_NONE &&
	    daemons_to_term == TRACKER_PROCESS_TYPE_NONE) {
		return 0;
	}

	pids = tracker_process_find_all ();

	str = g_strdup_printf (g_dngettext (NULL,
	                                    "Found %d PID…",
	                                    "Found %d PIDs…",
	                                    g_slist_length (pids)),
	                       g_slist_length (pids));
	g_print ("%s\n", str);
	g_free (str);

	for (l = pids; l; l = l->next) {
		TrackerProcessData *pd;
		const gchar *basename;
		pid_t pid;

		pd = l->data;
		basename = pd->cmd;
		pid = pd->pid;
		
		if (daemons_to_term == TRACKER_PROCESS_TYPE_MINERS) {
			if (!strstr (basename, "tracker-miner"))
				continue;

			if (kill (pid, SIGTERM) == -1) {
				const gchar *errstr = g_strerror (errno);
						
				str = g_strdup_printf (_("Could not terminate process %d — “%s”"), pid, basename);
				g_printerr ("  %s: %s\n",
				            str,
				            errstr ? errstr : _("No error given"));
				g_free (str);
			} else {
				str = g_strdup_printf (_("Terminated process %d — “%s”"), pid, basename);
				g_print ("  %s\n", str);
				g_free (str);
			}
		} else if (daemons_to_kill == TRACKER_PROCESS_TYPE_MINERS) {
			if (!strstr (basename, "tracker-miner"))
				continue;

			if (kill (pid, SIGKILL) == -1) {
				const gchar *errstr = g_strerror (errno);

				str = g_strdup_printf (_("Could not kill process %d — “%s”"), pid, basename);
				g_printerr ("  %s: %s\n",
				            str,
				            errstr ? errstr : _("No error given"));
				g_free (str);
			} else {
				str = g_strdup_printf (_("Killed process %d — “%s”"), pid, basename);
				g_print ("  %s\n", str);
				g_free (str);
			}
		}
	}

	g_slist_foreach (pids, (GFunc) tracker_process_data_free, NULL);
	g_slist_free (pids);

	return 0;
}
