/*
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
 * Copyright (C) 2021, Red Hat Inc.
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
 */

#include "config-miners.h"

#include <gio/gunixmounts.h>

#include "tracker-miner-files-methods.h"

#include <libtracker-extract/tracker-extract.h>
#include <libtracker-miners-common/tracker-common.h>

#define DEFAULT_GRAPH "tracker:FileSystem"

static void
miner_files_add_to_datasource (TrackerMinerFiles *mf,
                               GFile             *file,
                               TrackerResource   *resource,
                               TrackerResource   *element_resource)
{
	TrackerIndexingTree *indexing_tree;
	TrackerMinerFS *fs = TRACKER_MINER_FS (mf);

	indexing_tree = tracker_miner_fs_get_indexing_tree (fs);

	if (tracker_indexing_tree_file_is_root (indexing_tree, file)) {
		tracker_resource_set_relation (resource, "nie:dataSource", element_resource);
	} else {
		const gchar *identifier = NULL;
		GFile *root;

		root = tracker_indexing_tree_get_root (indexing_tree, file, NULL);

		if (root)
			identifier = tracker_miner_fs_get_identifier (fs, root);

		if (identifier)
			tracker_resource_set_uri (resource, "nie:dataSource", identifier);
	}
}

static void
miner_files_add_mount_info (TrackerMinerFiles *miner,
                            TrackerResource   *resource,
                            GFile             *file)
{
	TrackerStorage *storage;
	TrackerStorageType storage_type;

	storage = tracker_miner_files_get_storage (miner);
	storage_type = tracker_storage_get_type_for_file (storage, file);

	if (storage_type == 0)
		return;

	tracker_resource_set_boolean (resource, "tracker:isRemovable",
	                              (storage_type & TRACKER_STORAGE_REMOVABLE) != 0);
	tracker_resource_set_boolean (resource, "tracker:isOptical",
	                              (storage_type & TRACKER_STORAGE_OPTICAL) != 0);
}

static TrackerResource *
miner_files_create_folder_information_element (TrackerMinerFiles *miner,
                                               GFile             *file,
                                               const gchar       *mime_type,
                                               gboolean           create)
{
	TrackerResource *resource, *file_resource;
	TrackerIndexingTree *indexing_tree;
	const gchar *urn;
	gchar *uri;

	/* Preserve URN for nfo:Folders */
	urn = tracker_miner_fs_get_identifier (TRACKER_MINER_FS (miner),
	                                       file);
	resource = tracker_resource_new (urn);

	tracker_resource_set_string (resource, "nie:mimeType", mime_type);
	tracker_resource_add_uri (resource, "rdf:type", "nie:InformationElement");

	tracker_resource_add_uri (resource, "rdf:type", "nfo:Folder");
	indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (miner));

	if (tracker_indexing_tree_file_is_root (indexing_tree, file)) {
		tracker_resource_add_uri (resource, "rdf:type", "tracker:IndexedFolder");
		tracker_resource_set_boolean (resource, "tracker:available", TRUE);
		tracker_resource_set_uri (resource, "nie:rootElementOf",
		                          tracker_resource_get_identifier (resource));

		miner_files_add_mount_info (miner, resource, file);
	}

	uri = g_file_get_uri (file);
	file_resource = tracker_resource_new (uri);
	tracker_resource_add_uri (file_resource, "rdf:type", "nfo:FileDataObject");
	g_free (uri);

	/* Laying the link between the IE and the DO */
	tracker_resource_set_take_relation (resource, "nie:isStoredAs", file_resource);
	tracker_resource_set_uri (file_resource, "nie:interpretedAs",
				  tracker_resource_get_identifier (resource));

	return resource;
}

static TrackerResource *
miner_files_create_text_file_information_element (TrackerMinerFiles *miner,
                                                  GFile             *file,
                                                  const gchar       *mime_type)
{
	TrackerResource *resource;
	GStrv rdf_types;
	const gchar *urn;
	int i;

	urn = tracker_miner_fs_get_identifier (TRACKER_MINER_FS (miner),
	                                       file);
	resource = tracker_resource_new (urn);

	rdf_types = tracker_extract_module_manager_get_rdf_types (mime_type);

	for (i = 0; rdf_types[i]; i++)
		tracker_resource_add_uri (resource, "rdf:type", rdf_types[i]);

	g_strfreev (rdf_types);

	return resource;
}

static TrackerResource *
miner_files_create_empty_information_element (TrackerMinerFiles *miner,
                                              GFile             *file)
{
	TrackerResource *resource;
	const gchar *urn;

	urn = tracker_miner_fs_get_identifier (TRACKER_MINER_FS (miner),
	                                       file);
	resource = tracker_resource_new (urn);
	tracker_resource_add_uri (resource, "rdf:type", "nie:InformationElement");

	return resource;
}

gchar *
get_content_type (GFile     *file,
		  GFileInfo *file_info)
{
	g_autoptr (GFileInfo) content_info = NULL;

	if (!g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE)) {
		content_info =
			g_file_query_info (file,
					   G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
					   G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
					   NULL, NULL);
	}

	return g_strdup (g_file_info_get_content_type (content_info ? content_info : file_info));
}

void
tracker_miner_files_process_file (TrackerMinerFS      *fs,
                                  GFile               *file,
                                  GFileInfo           *file_info,
                                  TrackerSparqlBuffer *buffer,
                                  gboolean             create)
{
	TrackerIndexingTree *indexing_tree;
	g_autoptr (TrackerResource) resource = NULL, folder_resource = NULL, graph_file = NULL;
	const gchar *graph;
	const gchar *parent_urn;
	g_autoptr (GFile) parent = NULL;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *mime_type = NULL;
	gboolean is_directory, is_root;
	g_autoptr (GDateTime) modified = NULL;
	g_autoptr (GDateTime) accessed = NULL, created = NULL;

	mime_type = get_content_type (file, file_info);
	if (!mime_type)
		return;

	uri = g_file_get_uri (file);
	indexing_tree = tracker_miner_fs_get_indexing_tree (fs);

	is_root = tracker_indexing_tree_file_is_root (indexing_tree, file);
	is_directory = (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY ?
	                TRUE : FALSE);

	modified = g_file_info_get_modification_date_time (file_info);
	if (!modified)
		modified = g_date_time_new_from_unix_utc (0);

	resource = tracker_resource_new (uri);

	tracker_resource_add_uri (resource, "rdf:type", "nfo:FileDataObject");

	parent = g_file_get_parent (file);
	parent_urn = tracker_miner_fs_get_identifier (fs, parent);

	if (parent_urn) {
		tracker_resource_set_uri (resource, "nfo:belongsToContainer", parent_urn);
	}

	tracker_resource_set_string (resource, "nfo:fileName",
	                             g_file_info_get_display_name (file_info));
	tracker_resource_set_int64 (resource, "nfo:fileSize",
	                            g_file_info_get_size (file_info));

	tracker_resource_set_datetime (resource, "nfo:fileLastModified", modified);

#ifdef GIO_SUPPORTS_CREATION_TIME
	accessed = g_file_info_get_access_date_time (file_info);
	if (accessed)
		tracker_resource_set_datetime (resource, "nfo:fileLastAccessed", accessed);

	created = g_file_info_get_creation_date_time (file_info);
	if (created)
		tracker_resource_set_datetime (resource, "nfo:fileCreated", created);
#else
	time_ = (time_t) g_file_info_get_attribute_uint64 (file_info, G_FILE_ATTRIBUTE_TIME_ACCESS);
	accessed = g_date_time_new_from_unix_local (time_);
	if (accessed)
		tracker_resource_set_datetime (resource, "nfo:fileLastAccessed", accessed);
#endif

	/* The URL of the DataObject (because IE = DO, this is correct) */
	tracker_resource_set_string (resource, "nie:url", uri);

	if (is_directory) {
		folder_resource =
			miner_files_create_folder_information_element (TRACKER_MINER_FILES (fs),
			                                               file,
			                                               mime_type,
			                                               create);

		/* Always use inode/directory here, we don't really care if it's a symlink */
		tracker_resource_set_string (resource, "tracker:extractorHash",
		                             tracker_extract_module_manager_get_hash ("inode/directory"));
	}

	miner_files_add_to_datasource (TRACKER_MINER_FILES (fs), file, resource, folder_resource);

	graph = tracker_extract_module_manager_get_graph (mime_type);

	if (graph && g_file_info_get_size (file_info) > 0) {
		TrackerResource *information_element;

		/* This mimetype will be extracted by some module, pre-fill the
		 * nfo:FileDataObject in that graph.
		 * Empty files skipped as mime-type for those cannot be trusted.
		 */
		graph_file = tracker_resource_new (uri);
		tracker_resource_add_uri (graph_file, "rdf:type", "nfo:FileDataObject");

		tracker_resource_set_string (graph_file, "nfo:fileName",
		                             g_file_info_get_display_name (file_info));

		tracker_resource_set_datetime (graph_file, "nfo:fileLastModified", modified);

		tracker_resource_set_int64 (graph_file, "nfo:fileSize",
		                            g_file_info_get_size (file_info));
		miner_files_add_to_datasource (TRACKER_MINER_FILES (fs), file, graph_file, NULL);

		if (tracker_extract_module_manager_check_fallback_rdf_type (mime_type,
		                                                            "nfo:PlainTextDocument") &&
		    !tracker_miner_files_check_allowed_text_file (TRACKER_MINER_FILES (fs), file)) {
			/* We let disallowed text files have a shallow document nie:InformationElement */
			information_element =
				miner_files_create_text_file_information_element (TRACKER_MINER_FILES (fs),
				                                                  file, mime_type);

			tracker_resource_set_string (resource, "tracker:extractorHash",
			                             tracker_extract_module_manager_get_hash (mime_type));
		} else {
			/* Insert only the base nie:InformationElement class, for the extractor to get
			 * the suitable content identifier.
			 */
			information_element =
				miner_files_create_empty_information_element (TRACKER_MINER_FILES (fs),
				                                              file);
		}

		tracker_resource_set_take_relation (graph_file, "nie:interpretedAs", information_element);
		tracker_resource_set_uri (information_element, "nie:isStoredAs", uri);
	}

	if (is_directory) {
		tracker_sparql_buffer_log_folder (buffer, file,
		                                  is_root,
		                                  resource, folder_resource);
	} else {
		tracker_sparql_buffer_log_file (buffer, file,
		                                graph,
		                                resource,
		                                graph_file);
	}
}

void
tracker_miner_files_process_file_attributes (TrackerMinerFS      *fs,
                                             GFile               *file,
                                             GFileInfo           *info,
                                             TrackerSparqlBuffer *buffer)
{
	g_autoptr (TrackerResource) resource = NULL, graph_file = NULL;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *mime_type = NULL;
	const gchar *graph;
	g_autoptr (GDateTime) modified = NULL;
	g_autoptr (GDateTime) accessed = NULL, created = NULL;

	mime_type = get_content_type (file, info);
	if (!mime_type)
		return;

	uri = g_file_get_uri (file);
	resource = tracker_resource_new (uri);
	tracker_resource_add_uri (resource, "rdf:type", "nfo:FileDataObject");

	modified = g_file_info_get_modification_date_time (info);
	if (!modified)
		modified = g_date_time_new_from_unix_utc (0);

	graph = tracker_extract_module_manager_get_graph (mime_type);

	/* Update nfo:fileLastModified */
	tracker_resource_set_datetime (resource, "nfo:fileLastModified", modified);
	if (graph) {
		graph_file = tracker_resource_new (uri);
		tracker_resource_add_uri (graph_file, "rdf:type", "nfo:FileDataObject");
		tracker_resource_set_datetime (graph_file, "nfo:fileLastModified", modified);
	}

#ifdef GIO_SUPPORTS_CREATION_TIME
	/* Update nfo:fileLastAccessed */
	accessed = g_file_info_get_access_date_time (info);
	tracker_resource_set_datetime (resource, "nfo:fileLastAccessed", accessed);

	/* Update nfo:fileCreated */
	created = g_file_info_get_creation_date_time (info);

	if (created)
		tracker_resource_set_datetime (resource, "nfo:fileCreated", created);
#else
	time_ = (time_t) g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_ACCESS);
	accessed = g_date_time_new_from_unix_local (time_);
	if (accessed)
		tracker_resource_set_datetime (resource, "nfo:fileLastAccessed", accessed);
#endif

	tracker_sparql_buffer_log_attributes_update (buffer,
	                                             file,
	                                             graph,
	                                             resource,
	                                             graph_file);
}

static gchar *
lookup_filesystem_id (TrackerMinerFiles *files,
                      GFile             *file)
{
	const gchar *id = NULL, *devname = NULL;
	GUnixMountEntry *mount;
	GUdevClient *udev_client;
	g_autoptr (GUdevDevice) udev_device = NULL;

	mount = g_unix_mount_for (g_file_peek_path (file), NULL);
	if (mount)
		devname = g_unix_mount_get_device_path (mount);

	if (devname) {
		udev_client = tracker_miner_files_get_udev_client (files);
		udev_device = g_udev_client_query_by_device_file (udev_client, devname);
		if (udev_device) {
			id = g_udev_device_get_property (udev_device, "ID_FS_UUID_SUB");
			if (!id)
				id = g_udev_device_get_property (udev_device, "ID_FS_UUID");
		}
	}

	return g_strdup (id);
}

gchar *
tracker_miner_files_get_content_identifier (TrackerMinerFiles *mf,
                                            GFile             *file,
                                            GFileInfo         *info)
{
	g_autofree gchar *inode = NULL, *str = NULL, *id = NULL;

	id = lookup_filesystem_id (mf, file);

	if (!id) {
		id = g_strdup (g_file_info_get_attribute_string (info,
		                                                 G_FILE_ATTRIBUTE_ID_FILESYSTEM));
	}

	inode = g_file_info_get_attribute_as_string (info, G_FILE_ATTRIBUTE_UNIX_INODE);

	/* Format:
	 * 'urn:fileid:' [uuid] ':' [inode]
	 */
	str = g_strconcat ("urn:fileid:", id, ":", inode, NULL);

	return g_steal_pointer (&str);
}
