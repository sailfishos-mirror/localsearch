/*
 * Copyright (C) 2011, Nokia <ivan.frade@nokia.com>
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
 * Author: Carlos Garnacho  <carlos@lanedo.com>
 */

#include "config-miners.h"

#include "tracker-indexing-tree.h"

#include "tracker-indexing-tree-methods.h"

#include <tracker-common.h>

/**
 * SECTION:tracker-indexing-tree
 * @short_description: Indexing tree handling
 *
 * #TrackerIndexingTree handles the tree of directories configured to be indexed
 * by the #TrackerMinerFS.
 **/

typedef struct _ConfiguredFolder ConfiguredFolder;
typedef struct _PatternData PatternData;

struct _ConfiguredFolder
{
	GFile *file;
	char *id;
	TrackerDirectoryFlags flags;
	int uri_len;
};

struct _PatternData
{
	GPatternSpec *pattern;
	gchar *string;
	TrackerFilterType type;
};

struct _TrackerIndexingTree
{
	GObject parent_instance;

	GArray *configured_folders;
	GList *filter_patterns;
	GList *allowed_text_patterns;

	guint filter_hidden : 1;
};

G_DEFINE_TYPE (TrackerIndexingTree, tracker_indexing_tree, G_TYPE_OBJECT)

enum {
	PROP_0,
	PROP_FILTER_HIDDEN
};

enum {
	DIRECTORY_ADDED,
	DIRECTORY_REMOVED,
	DIRECTORY_UPDATED,
	CHILD_UPDATED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
configured_folder_clear (gpointer data)
{
	ConfiguredFolder *folder = data;

	g_clear_object (&folder->file);
	g_clear_pointer (&folder->id, g_free);
}

static ConfiguredFolder *
find_configured_folder (TrackerIndexingTree *tree,
			GFile               *file,
			GEqualFunc           func,
			unsigned int        *pos)
{
	unsigned int i;

	for (i = 0; i < tree->configured_folders->len; i++) {
		ConfiguredFolder *folder;

		folder = &g_array_index (tree->configured_folders, ConfiguredFolder, i);

		if (func (file, folder->file)) {
			if (pos)
				*pos = i;

			return folder;
		}
	}

	return NULL;
}

static PatternData *
pattern_data_new (const gchar *string,
                  guint        type)
{
	PatternData *data;

	data = g_slice_new0 (PatternData);
	data->type = type;

	switch (type) {
	case TRACKER_FILTER_FILE:
	case TRACKER_FILTER_DIRECTORY:
		data->pattern = g_pattern_spec_new (string);
		data->string = g_strdup (string);
		break;
	case TRACKER_FILTER_PARENT_DIRECTORY:
		data->string = g_strdup (string);
		break;
	}

	return data;
}

static void
pattern_data_free (PatternData *data)
{
	g_clear_pointer (&data->pattern, g_pattern_spec_free);
	g_free (data->string);
	g_slice_free (PatternData, data);
}

static int
pattern_data_compare (const PatternData *data1,
                      const PatternData *data2)
{
	if (data1->type != data2->type)
		return -1;

	return g_strcmp0 (data2->string, data2->string);
}

static void
tracker_indexing_tree_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
	TrackerIndexingTree *tree = TRACKER_INDEXING_TREE (object);

	switch (prop_id) {
	case PROP_FILTER_HIDDEN:
		g_value_set_boolean (value, tree->filter_hidden);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
tracker_indexing_tree_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
	TrackerIndexingTree *tree;

	tree = TRACKER_INDEXING_TREE (object);

	switch (prop_id) {
	case PROP_FILTER_HIDDEN:
		tracker_indexing_tree_set_filter_hidden (tree,
		                                         g_value_get_boolean (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
tracker_indexing_tree_finalize (GObject *object)
{
	TrackerIndexingTree *tree;

	tree = TRACKER_INDEXING_TREE (object);

	g_clear_list (&tree->allowed_text_patterns, (GDestroyNotify) pattern_data_free);
	g_clear_list (&tree->filter_patterns, (GDestroyNotify) pattern_data_free);
	g_clear_pointer (&tree->configured_folders, g_array_unref);

	G_OBJECT_CLASS (tracker_indexing_tree_parent_class)->finalize (object);
}

static void
tracker_indexing_tree_class_init (TrackerIndexingTreeClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_indexing_tree_finalize;
	object_class->set_property = tracker_indexing_tree_set_property;
	object_class->get_property = tracker_indexing_tree_get_property;

	g_object_class_install_property (object_class,
	                                 PROP_FILTER_HIDDEN,
	                                 g_param_spec_boolean ("filter-hidden",
	                                                       "Filter hidden",
	                                                       "Whether hidden resources are filtered",
	                                                       FALSE,
	                                                       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	/**
	 * TrackerIndexingTree::directory-added:
	 * @indexing_tree: a #TrackerIndexingTree
	 * @directory: a #GFile
	 *
	 * the ::directory-added signal is emitted when a new
	 * directory is added to the list of other directories which
	 * are to be considered for indexing. Typically this is
	 * signalled when the tracker_indexing_tree_add() API is
	 * called.
	 *
	 * Since: 0.14
	 **/
	signals[DIRECTORY_ADDED] =
		g_signal_new ("directory-added",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              0, NULL, NULL, NULL,
		              G_TYPE_NONE, 1, G_TYPE_FILE);

	/**
	 * TrackerIndexingTree::directory-removed:
	 * @indexing_tree: a #TrackerIndexingTree
	 * @directory: a #GFile
	 *
	 * the ::directory-removed signal is emitted when a
	 * directory is removed from the list of other directories
	 * which are to be considered for indexing. Typically this is
	 * signalled when the tracker_indexing_tree_remove() API is
	 * called.
	 *
	 * Since: 0.14
	 **/
	signals[DIRECTORY_REMOVED] =
		g_signal_new ("directory-removed",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              0, NULL, NULL, NULL,
		              G_TYPE_NONE, 1, G_TYPE_FILE);

	/**
	 * TrackerIndexingTree::directory-updated:
	 * @indexing_tree: a #TrackerIndexingTree
	 * @directory: a #GFile
	 *
	 * The ::directory-updated signal is emitted on a root
	 * when either its indexing flags change (e.g. due to consecutive
	 * calls to tracker_indexing_tree_add()), or anytime an update is
	 * requested through tracker_indexing_tree_notify_update().
	 *
	 * Since: 0.14
	 **/
	signals[DIRECTORY_UPDATED] =
		g_signal_new ("directory-updated",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              0, NULL, NULL, NULL,
		              G_TYPE_NONE, 1, G_TYPE_FILE);

	/**
	 * TrackerIndexingTree::child-updated:
	 * @indexing_tree: a #TrackerIndexingTree
	 * @root: the root of this child
	 * @child: the updated child
	 *
	 * The ::child-updated signal may be emitted to notify
	 * about possible changes on children of a root.
	 *
	 * #TrackerIndexingTree does not emit those by itself,
	 * those may be triggered through tracker_indexing_tree_notify_update().
	 *
	 * Since: 1.10
	 **/
	signals[CHILD_UPDATED] =
		g_signal_new ("child-updated",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              0, NULL, NULL, NULL,
		              G_TYPE_NONE, 2, G_TYPE_FILE, G_TYPE_FILE);
}

static void
tracker_indexing_tree_init (TrackerIndexingTree *tree)
{
	tree->configured_folders =
		g_array_new (FALSE, FALSE, sizeof (ConfiguredFolder));
	g_array_set_clear_func (tree->configured_folders,
				configured_folder_clear);
}

/**
 * tracker_indexing_tree_new:
 *
 * Returns a newly created #TrackerIndexingTree
 *
 * Returns: a newly allocated #TrackerIndexingTree
 *
 * Since: 0.14
 **/
TrackerIndexingTree *
tracker_indexing_tree_new (void)
{
	return g_object_new (TRACKER_TYPE_INDEXING_TREE, NULL);
}

/**
 * tracker_indexing_tree_add:
 * @tree: a #TrackerIndexingTree
 * @directory: #GFile pointing to a directory
 * @flags: Configuration flags for the directory
 *
 * Adds a directory to the indexing tree with the
 * given configuration flags.
 **/
void
tracker_indexing_tree_add (TrackerIndexingTree   *tree,
                           GFile                 *directory,
                           TrackerDirectoryFlags  flags)
{
	g_autofree char *uri = NULL;
	ConfiguredFolder new_folder;
	unsigned int i, pos = 0;
	int uri_len;
	gboolean insert = FALSE;

	g_return_if_fail (TRACKER_IS_INDEXING_TREE (tree));
	g_return_if_fail (G_IS_FILE (directory));

	uri = g_file_get_uri (directory);
	uri_len = strlen (uri);

	/* Elements are ordered by uri length, the principle is that
	 * in case of nesting of configured folders, deeper folders will
	 * come up first, so ordered g_file_has_prefix checks will be
	 * guaranteed to get the deepmost configured folder that applies.
	 */
	for (i = 0; i < tree->configured_folders->len; i++) {
		ConfiguredFolder *folder;

		folder = &g_array_index (tree->configured_folders, ConfiguredFolder, i);

		if (folder->uri_len > uri_len) {
			continue;
		} else if (folder->uri_len == uri_len) {
			if (g_file_equal (folder->file, directory)) {
				/* Overwrite flags if they are different */
				if (folder->flags != flags) {
					folder->flags = flags;
					g_signal_emit (tree, signals[DIRECTORY_UPDATED], 0,
						       folder->file);
				}

				/* Folder already existed */
				return;
			}

			continue;
		} else {
			/* Found the first shorter configured folder, thus the insertion spot */
			pos = i;
			insert = TRUE;
			break;
		}
	}

	new_folder = (ConfiguredFolder) {
		g_object_ref (directory),
		NULL, flags, uri_len,
	};

	if (insert)
		g_array_insert_val (tree->configured_folders, pos, new_folder);
	else
		g_array_append_val (tree->configured_folders, new_folder);

	g_signal_emit (tree, signals[DIRECTORY_ADDED], 0, directory);
}

/**
 * tracker_indexing_tree_remove:
 * @tree: a #TrackerIndexingTree
 * @directory: #GFile pointing to a directory
 *
 * Removes @directory from the indexing tree, note that
 * only directories previously added with tracker_indexing_tree_add()
 * can be effectively removed.
 **/
void
tracker_indexing_tree_remove (TrackerIndexingTree *tree,
                              GFile               *directory)
{
	ConfiguredFolder *folder;
	unsigned int pos;

	g_return_if_fail (TRACKER_IS_INDEXING_TREE (tree));
	g_return_if_fail (G_IS_FILE (directory));

	folder = find_configured_folder (tree, directory, (GEqualFunc) g_file_equal, &pos);
	if (!folder)
		return;

	g_signal_emit (tree, signals[DIRECTORY_REMOVED], 0, folder->file);
	g_array_remove_index (tree->configured_folders, pos);
}

/**
 * tracker_indexing_tree_notify_update:
 * @tree: a #TrackerIndexingTree
 * @file: a #GFile
 * @recursive: Whether contained indexing roots are affected by the update
 *
 * Signals either #TrackerIndexingTree::directory-updated or
 * #TrackerIndexingTree::child-updated on the given file and
 * returns #TRUE. If @file is not indexed according to the
 * #TrackerIndexingTree, #FALSE is returned.
 *
 * If @recursive is #TRUE, #TrackerIndexingTree::directory-updated
 * will be emitted on the indexing roots that are contained in @file.
 *
 * Returns: #TRUE if a signal is emitted.
 *
 * Since: 1.10
 **/
gboolean
tracker_indexing_tree_notify_update (TrackerIndexingTree *tree,
                                     GFile               *file,
                                     gboolean             recursive)
{
	TrackerDirectoryFlags flags;
	gboolean emitted = FALSE;
	GFile *root;

	g_return_val_if_fail (TRACKER_IS_INDEXING_TREE (tree), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	root = tracker_indexing_tree_get_root (tree, file, NULL, &flags);

	if (tracker_indexing_tree_file_is_root (tree, file)) {
		g_signal_emit (tree, signals[DIRECTORY_UPDATED], 0, root);
		emitted = TRUE;
	} else if (root &&
	           ((flags & TRACKER_DIRECTORY_FLAG_RECURSE) ||
	            g_file_has_parent (file, root))) {
		g_signal_emit (tree, signals[CHILD_UPDATED], 0, root, file);
		emitted = TRUE;
	}

	if (recursive) {
		GList *roots, *l;

		roots = tracker_indexing_tree_list_roots (tree);

		for (l = roots; l; l = l->next) {
			if (!g_file_has_prefix (l->data, file))
				continue;

			g_signal_emit (tree, signals[DIRECTORY_UPDATED], 0, l->data);
			emitted = TRUE;
		}

		g_list_free (roots);
	}

	return emitted;
}

/**
 * tracker_indexing_tree_add_filter:
 * @tree: a #TrackerIndexingTree
 * @filter: filter type
 * @glob_string: glob-style string for the filter
 *
 * Adds a new filter for basenames.
 **/
void
tracker_indexing_tree_add_filter (TrackerIndexingTree *tree,
                                  TrackerFilterType    filter,
                                  const gchar         *glob_string)
{
	PatternData *data;

	g_return_if_fail (TRACKER_IS_INDEXING_TREE (tree));
	g_return_if_fail (glob_string != NULL);

	if (g_path_is_absolute (glob_string)) {
		g_warning ("Absolute paths are no longer allowed in 'ignored-files', 'ignored-directories', or 'ignored-directories-with-content'");
		return;
	}

	if (filter == TRACKER_FILTER_PARENT_DIRECTORY && g_utf8_strchr (glob_string, -1, '*')) {
		g_warning ("Glob strings are no longer allowed in 'ignored-directories-with-content'");
		return;
	}

	data = pattern_data_new (glob_string, filter);
	tree->filter_patterns = g_list_prepend (tree->filter_patterns, data);
}

/**
 * tracker_indexing_tree_clear_filters:
 * @tree: a #TrackerIndexingTree
 * @type: filter type to clear
 *
 * Clears all filters of a given type.
 **/
void
tracker_indexing_tree_clear_filters (TrackerIndexingTree *tree,
                                     TrackerFilterType    type)
{
	GList *l;

	g_return_if_fail (TRACKER_IS_INDEXING_TREE (tree));

	for (l = tree->filter_patterns; l; l = l->next) {
		PatternData *data = l->data;

		if (data->type == type) {
			/* When we delete the link 'l', we point back
			 * to the beginning of the list to make sure
			 * we don't miss anything.
			 */
			l = tree->filter_patterns = g_list_delete_link (tree->filter_patterns, l);
			pattern_data_free (data);
		}
	}
}

/**
 * tracker_indexing_tree_file_matches_filter:
 * @tree: a #TrackerIndexingTree
 * @type: filter type
 * @file: a #GFile
 *
 * Returns %TRUE if @file matches any filter of the given filter type.
 *
 * Returns: %TRUE if @file is filtered.
 **/
gboolean
tracker_indexing_tree_file_matches_filter (TrackerIndexingTree *tree,
                                           TrackerFilterType    type,
                                           GFile               *file)
{
	GList *filters;
	gchar *basename, *str, *reverse;
	gboolean match = FALSE;
	gint len;

	g_return_val_if_fail (TRACKER_IS_INDEXING_TREE (tree), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	filters = tree->filter_patterns;
	basename = g_file_get_basename (file);

	str = g_utf8_make_valid (basename, -1);
	len = strlen (str);
	reverse = g_utf8_strreverse (str, len);

	while (filters) {
		PatternData *data = filters->data;

		filters = filters->next;

		if (data->type != type)
			continue;

		if (!data->pattern) {
			match = g_strcmp0 (str, data->string) == 0;
			break;
		}

#if GLIB_CHECK_VERSION (2, 70, 0)
		if (g_pattern_spec_match (data->pattern, len, str, reverse))
#else
		if (g_pattern_match (data->pattern, len, str, reverse))
#endif

		{
			match = TRUE;
			break;
		}
	}

	g_free (basename);
	g_free (str);
	g_free (reverse);

	return match;
}

static gboolean
parent_or_equals (GFile *file1,
                  GFile *file2)
{
	return (file1 == file2 ||
	        g_file_equal (file1, file2) ||
	        g_file_has_prefix (file1, file2));
}

/**
 * tracker_indexing_tree_file_is_indexable:
 * @tree: a #TrackerIndexingTree
 * @file: a #GFile
 * @file_info: a #GFileInfo
 *
 * returns %TRUE if @file should be indexed according to the
 * parameters given through tracker_indexing_tree_add() and
 * tracker_indexing_tree_add_filter().
 *
 * If @file_info is %NULL, it will be queried to the
 * file system.
 *
 * Returns: %TRUE if @file should be indexed.
 **/
gboolean
tracker_indexing_tree_file_is_indexable (TrackerIndexingTree *tree,
                                         GFile               *file,
                                         GFileInfo           *file_info)
{
	TrackerFilterType filter;
	TrackerDirectoryFlags config_flags;
	g_autoptr (GFileInfo) info = NULL;
	GFile *config_file;
	GFileType file_type;

	g_return_val_if_fail (TRACKER_IS_INDEXING_TREE (tree), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	config_file = tracker_indexing_tree_get_root (tree, file, NULL, &config_flags);
	if (!config_file) {
		/* Not under an added dir */
		return FALSE;
	}

	g_set_object (&info, file_info);

	if (info == NULL) {
		info = g_file_query_info (file,
		                          G_FILE_ATTRIBUTE_STANDARD_TYPE ","
		                          G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
		                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
		                          NULL, NULL);
	}

	file_type = info ? g_file_info_get_file_type (info) : G_FILE_TYPE_UNKNOWN;

	filter = (file_type == G_FILE_TYPE_DIRECTORY) ?
		TRACKER_FILTER_DIRECTORY : TRACKER_FILTER_FILE;

	if (tracker_indexing_tree_file_matches_filter (tree, filter, file)) {
		return FALSE;
	}

	if (g_file_equal (file, config_file)) {
		return TRUE;
	} else {
		if ((config_flags & TRACKER_DIRECTORY_FLAG_RECURSE) == 0 &&
		    !g_file_has_parent (file, config_file)) {
			/* Non direct child in a non-recursive dir, ignore */
			return FALSE;
		}

		if (tracker_indexing_tree_get_filter_hidden (tree) &&
		    info && g_file_info_get_is_hidden (info)) {
			return FALSE;
		}

		return TRUE;
	}
}

/**
 * tracker_indexing_tree_parent_is_indexable:
 * @tree: a #TrackerIndexingTree
 * @parent: directory
 *
 * returns %TRUE if @parent should be indexed based on its contents.
 *
 * Returns: %TRUE if @parent should be indexed.
 **/
gboolean
tracker_indexing_tree_parent_is_indexable (TrackerIndexingTree *tree,
                                           GFile               *parent)
{
	gboolean has_match = FALSE;
	GList *filters;

	g_return_val_if_fail (TRACKER_IS_INDEXING_TREE (tree), FALSE);
	g_return_val_if_fail (G_IS_FILE (parent), FALSE);

	filters = tree->filter_patterns;

	while (filters) {
		PatternData *data = filters->data;
		g_autoptr (GFile) child = NULL;

		filters = filters->next;

		if (data->type != TRACKER_FILTER_PARENT_DIRECTORY)
			continue;

		child = g_file_get_child (parent, data->string);

		if (g_file_query_exists (child, NULL)) {
			has_match = TRUE;
			break;
		}
	}

	return !has_match;
}

/**
 * tracker_indexing_tree_get_filter_hidden:
 * @tree: a #TrackerIndexingTree
 *
 * Describes if the @tree should index hidden content. To change this
 * setting, see tracker_indexing_tree_set_filter_hidden().
 *
 * Returns: %FALSE if hidden files are indexed, otherwise %TRUE.
 *
 * Since: 0.18
 **/
gboolean
tracker_indexing_tree_get_filter_hidden (TrackerIndexingTree *tree)
{
	g_return_val_if_fail (TRACKER_IS_INDEXING_TREE (tree), FALSE);

	return tree->filter_hidden;
}

/**
 * tracker_indexing_tree_set_filter_hidden:
 * @tree: a #TrackerIndexingTree
 * @filter_hidden: a boolean
 *
 * When indexing content, sometimes it is preferable to ignore hidden
 * content, for example, files prefixed with &quot;.&quot;. This is
 * common for files in a home directory which are usually config
 * files.
 *
 * Sets the indexing policy for @tree with hidden files and content.
 * To ignore hidden files, @filter_hidden should be %TRUE, otherwise
 * %FALSE.
 *
 * Since: 0.18
 **/
void
tracker_indexing_tree_set_filter_hidden (TrackerIndexingTree *tree,
                                         gboolean             filter_hidden)
{
	g_return_if_fail (TRACKER_IS_INDEXING_TREE (tree));

	tree->filter_hidden = filter_hidden;

	g_object_notify (G_OBJECT (tree), "filter-hidden");
}

GFile *
tracker_indexing_tree_get_root (TrackerIndexingTree    *tree,
                                GFile                  *file,
                                const char            **id,
                                TrackerDirectoryFlags  *directory_flags)
{
	ConfiguredFolder *folder;

	if (directory_flags) {
		*directory_flags = TRACKER_DIRECTORY_FLAG_NONE;
	}

	g_return_val_if_fail (TRACKER_IS_INDEXING_TREE (tree), NULL);
	g_return_val_if_fail (G_IS_FILE (file), NULL);

	folder = find_configured_folder (tree, file, (GEqualFunc) parent_or_equals, NULL);

	if (!folder)
		return NULL;

	if (!folder->id) {
		folder->id = tracker_indexing_tree_get_root_id (tree,
								folder->file);
	}

	if (id)
		*id = folder->id;
	if (directory_flags)
		*directory_flags = folder->flags;

	return folder->file;
}

/**
 * tracker_indexing_tree_file_is_root:
 * @tree: a #TrackerIndexingTree
 * @file: a #GFile to compare
 *
 * Evaluates if the URL represented by @file is the same of that for
 * the root of the @tree.
 *
 * Returns: %TRUE if @file matches the URL canonically, otherwise %FALSE.
 *
 * Since: 1.2
 **/
gboolean
tracker_indexing_tree_file_is_root (TrackerIndexingTree *tree,
                                    GFile               *file)
{
	g_return_val_if_fail (TRACKER_IS_INDEXING_TREE (tree), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	return find_configured_folder (tree, file, (GEqualFunc) g_file_equal, NULL) != NULL;
}

/**
 * tracker_indexing_tree_list_roots:
 * @tree: a #TrackerIndexingTree
 *
 * Returns the list of indexing roots in @tree
 *
 * Returns: (transfer container) (element-type GFile): The list
 *          of roots, the list itself must be freed with g_list_free(),
 *          the list elements are owned by @tree and should not be
 *          freed.
 **/
GList *
tracker_indexing_tree_list_roots (TrackerIndexingTree *tree)
{
	unsigned int i;
	GList *roots = NULL;

	g_return_val_if_fail (TRACKER_IS_INDEXING_TREE (tree), NULL);

	for (i = 0; i < tree->configured_folders->len; i++) {
		ConfiguredFolder *folder;

		folder = &g_array_index (tree->configured_folders, ConfiguredFolder, i);
		roots = g_list_prepend (roots, folder->file);
	}

	return roots;
}

void
tracker_indexing_tree_clear_allowed_text_patterns (TrackerIndexingTree *tree)
{
	g_clear_list (&tree->allowed_text_patterns, (GDestroyNotify) pattern_data_free);
}

void
tracker_indexing_tree_add_allowed_text_pattern (TrackerIndexingTree *tree,
                                                const char          *pattern_str)
{
	PatternData *pattern;

	pattern = pattern_data_new (pattern_str, 0);
	tree->allowed_text_patterns =
		g_list_prepend (tree->allowed_text_patterns, pattern);
}

gboolean
tracker_indexing_tree_file_has_allowed_text_extension (TrackerIndexingTree *tree,
                                                       GFile               *file)
{
	g_autofree gchar *basename = NULL;
	GList *l;

	basename = g_file_get_basename (file);

	for (l = tree->allowed_text_patterns; l; l = l->next) {
		PatternData *pattern = l->data;

#if GLIB_CHECK_VERSION (2, 70, 0)
		if (g_pattern_spec_match_string (pattern->pattern, basename))
#else
		if (g_pattern_match_string (pattern->pattern, basename))
#endif
			return TRUE;
	}

	return FALSE;
}

void
tracker_indexing_tree_update_all (TrackerIndexingTree *tree)
{
	GList *roots, *l;

	roots = tracker_indexing_tree_list_roots (tree);

	for (l = roots; l; l = l->next) {
		GFile *root = l->data;

		tracker_indexing_tree_notify_update (tree, root, FALSE);
	}

	g_list_free (roots);
}

gboolean
tracker_indexing_tree_save_config (TrackerIndexingTree  *tree,
                                   GFile                *config,
                                   GError              **error)
{
	GVariantBuilder builder, text_allowlist, single_dirs, recursive_dirs;
	GVariantBuilder ignored_files, ignored_dirs, ignored_dirs_with_content;
	g_autoptr (GVariant) variant = NULL;
	g_autofree char *str = NULL;
	GList *l, *roots;

	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
	g_variant_builder_init (&text_allowlist, G_VARIANT_TYPE ("as"));
	g_variant_builder_init (&ignored_files, G_VARIANT_TYPE ("as"));
	g_variant_builder_init (&ignored_dirs, G_VARIANT_TYPE ("as"));
	g_variant_builder_init (&ignored_dirs_with_content, G_VARIANT_TYPE ("as"));
	g_variant_builder_init (&single_dirs, G_VARIANT_TYPE ("as"));
	g_variant_builder_init (&recursive_dirs, G_VARIANT_TYPE ("as"));

	for (l = tree->allowed_text_patterns; l; l = l->next) {
		PatternData *pattern = l->data;
		g_variant_builder_add (&text_allowlist, "s", pattern->string);
	}

	for (l = tree->filter_patterns; l; l = l->next) {
		PatternData *pattern = l->data;

		if (pattern->type == TRACKER_FILTER_FILE)
			g_variant_builder_add (&ignored_files, "s", pattern->string);
		else if (pattern->type == TRACKER_FILTER_DIRECTORY)
			g_variant_builder_add (&ignored_dirs, "s", pattern->string);
		else if (pattern->type == TRACKER_FILTER_PARENT_DIRECTORY)
			g_variant_builder_add (&ignored_dirs_with_content, "s", pattern->string);
	}

	roots = tracker_indexing_tree_list_roots (tree);

	for (l = roots; l; l = l->next) {
		TrackerDirectoryFlags flags;

		tracker_indexing_tree_get_root (tree,
		                                l->data, NULL, &flags);

		if (!!(flags & TRACKER_DIRECTORY_FLAG_RECURSE)) {
			g_variant_builder_add (&recursive_dirs, "s",
			                       g_file_peek_path (l->data));
		} else {
			g_variant_builder_add (&single_dirs, "s",
			                       g_file_peek_path (l->data));
		}
	}

	g_list_free (roots);

	g_variant_builder_add (&builder, "{sv}", "text-allowlist",
	                       g_variant_builder_end (&text_allowlist));
	g_variant_builder_add (&builder, "{sv}", "ignored-files",
	                       g_variant_builder_end (&ignored_files));
	g_variant_builder_add (&builder, "{sv}", "ignored-directories",
	                       g_variant_builder_end (&ignored_dirs));
	g_variant_builder_add (&builder, "{sv}", "ignored-directories-with-content",
	                       g_variant_builder_end (&ignored_dirs_with_content));
	g_variant_builder_add (&builder, "{sv}", "index-single-directories",
	                       g_variant_builder_end (&single_dirs));
	g_variant_builder_add (&builder, "{sv}", "index-recursive-directories",
	                       g_variant_builder_end (&recursive_dirs));

	variant = g_variant_builder_end (&builder);
	str = g_variant_print (variant, FALSE);

	return g_file_set_contents (g_file_peek_path (config), str, -1, error);
}

static gboolean
compare_filter (TrackerIndexingTree *tree,
                GVariant            *variant,
                const char          *key,
                TrackerFilterType    type,
                int                 *n_items_found_inout)
{
	g_auto (GStrv) strv = NULL;
	int n_filters = 0, i;

	if (g_variant_lookup (variant, key, "^as", &strv)) {
		for (i = 0; strv[i]; i++) {
			PatternData match = { 0, };

			match.type = type;
			match.string = strv[i];

			if (!g_list_find_custom (tree->filter_patterns, &match,
			                         (GCompareFunc) pattern_data_compare))
				return FALSE;

			n_filters++;
		}

		g_clear_pointer (&strv, g_strfreev);
	}

	(*n_items_found_inout) += n_filters;
	return TRUE;
}

static gboolean
compare_directories (TrackerIndexingTree   *tree,
                     GVariant              *variant,
                     const char            *key,
                     TrackerDirectoryFlags  mask,
                     TrackerDirectoryFlags  value,
                     int                   *n_roots_inout)
{
	g_auto (GStrv) strv = NULL;
	int n_roots = 0, i;

	if (g_variant_lookup (variant, key, "^as", &strv)) {
		for (i = 0; strv[i]; i++) {
			g_autoptr (GFile) file = NULL;
			TrackerDirectoryFlags flags;
			GFile *root;

			file = g_file_new_for_path (strv[i]);
			root = tracker_indexing_tree_get_root (tree, file, NULL, &flags);

			if (!root || !g_file_equal (file, root) ||
			    (flags & mask) != value)
				return FALSE;

			n_roots++;
		}

		g_clear_pointer (&strv, g_strfreev);
	}

	(*n_roots_inout) += n_roots;
	return TRUE;
}

gboolean
tracker_indexing_tree_check_config (TrackerIndexingTree *tree,
                                    GFile               *config,
                                    gboolean             check_locations)
{
	g_autoptr (GVariant) variant = NULL;
	g_autofree char *str = NULL;
	g_auto (GStrv) strv = NULL;
	GList *roots;
	int i, n_filters = 0, n_roots = 0, cur_n_roots;
	size_t len;

	if (!g_file_get_contents (g_file_peek_path (config), &str, &len, NULL))
		goto update;

	variant = g_variant_parse (G_VARIANT_TYPE ("a{sv}"),
	                           str, &str[len], NULL, NULL);
	if (!variant)
		goto update;

	if (g_variant_lookup (variant, "text-allowlist", "^as", &strv)) {
		for (i = 0; strv[i]; i++) {
			PatternData match = { 0, };

			match.string = strv[i];

			if (!g_list_find_custom (tree->allowed_text_patterns, &match,
			                         (GCompareFunc) pattern_data_compare))
				goto update;
		}

		if (g_strv_length (strv) != g_list_length (tree->allowed_text_patterns))
			goto update;

		g_clear_pointer (&strv, g_strfreev);
	} else if (tree->allowed_text_patterns) {
		goto update;
	}

	if (!compare_filter (tree, variant, "ignored-files",
	                     TRACKER_FILTER_FILE, &n_filters))
		goto update;

	if (!compare_filter (tree, variant, "ignored-directories",
	                     TRACKER_FILTER_DIRECTORY, &n_filters))
		goto update;

	if (!compare_filter (tree, variant, "ignored-directories-with-content",
	                     TRACKER_FILTER_PARENT_DIRECTORY, &n_filters))
		goto update;

	if (n_filters != g_list_length (tree->filter_patterns))
		goto update;

	if (check_locations) {
		if (!compare_directories (tree, variant, "index-single-directories",
		                          TRACKER_DIRECTORY_FLAG_RECURSE,
		                          TRACKER_DIRECTORY_FLAG_NONE,
		                          &n_roots))
			goto update;

		if (!compare_directories (tree, variant, "index-recursive-directories",
		                          TRACKER_DIRECTORY_FLAG_RECURSE,
		                          TRACKER_DIRECTORY_FLAG_RECURSE,
		                          &n_roots))
			goto update;

		roots = tracker_indexing_tree_list_roots (tree);
		cur_n_roots = g_list_length (roots);
		g_list_free (roots);

		if (n_roots != cur_n_roots)
			goto update;
	}

	/* Everything matches, nothing to do */
	return TRUE;

 update:
	tracker_indexing_tree_update_all (tree);
	return FALSE;
}
