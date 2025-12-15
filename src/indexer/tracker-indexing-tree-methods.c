/*
 * Copyright (C) 2025, Red Hat Inc.
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

#include "tracker-indexing-tree-methods.h"

#include <glib/gstdio.h>

#include <fcntl.h>
#include <sys/ioctl.h>

#ifdef __linux__
#include <linux/btrfs.h>
#endif

#define UUID_FORMAT_STR "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x"

#define UUID_FORMAT_ARGS(bytes)	                    \
	bytes[0],  bytes[1],  bytes[2],  bytes[3],  \
	bytes[4],  bytes[5],  bytes[6],  bytes[7],  \
	bytes[8],  bytes[9],  bytes[10], bytes[11], \
	bytes[12], bytes[13], bytes[14], bytes[15]

char *
tracker_indexing_tree_get_root_id (TrackerIndexingTree *tree,
                                   GFile               *root)
{
	g_autofree char *path = NULL, *expanded = NULL;
	char *id = NULL;
	g_autofd int fd;

	path = g_file_get_path (root);
	expanded = realpath (path, NULL);

	fd = open (expanded ? expanded : path, O_RDONLY | O_DIRECTORY);

#ifdef HAVE_BTRFS_IOCTL
	{
		struct btrfs_ioctl_get_subvol_info_args subvol_info;

		if (fd >= 0 && ioctl (fd, BTRFS_IOC_GET_SUBVOL_INFO, &subvol_info) >= 0) {
			id = g_strdup_printf (UUID_FORMAT_STR,
			                      UUID_FORMAT_ARGS (subvol_info.uuid));
		}
	}
#endif
#ifdef __linux__
	if (!id) {
		struct fsuuid2 uuid;

		if (ioctl (fd, FS_IOC_GETFSUUID, &uuid) >= 0 && uuid.len == 16) {
			id = g_strdup_printf (UUID_FORMAT_STR,
			                      UUID_FORMAT_ARGS (uuid.uuid));
		}
	}
#endif

	return id;
}
