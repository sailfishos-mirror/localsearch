/*
 * Copyright (C) 2010, Nokia <ivan.frade@nokia.com>
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

#include <string.h>

#include "tracker-extract-rules-manager.h"

#include <tracker-common.h>

static void tracker_extract_rules_manager_initable_iface_init (GInitableIface *iface);

typedef struct {
	gchar *rule_path;
	gchar *module_path;
	GList *allow_patterns;
	GList *block_patterns;
	GStrv fallback_rdf_types;
	gchar *graph;
	gchar *hash;
} RuleInfo;

typedef struct {
	const GList *rules;
	const GList *cur;
} TrackerMimetypeInfo;

struct _TrackerExtractRulesManager
{
	GObject parent_instance;

	GHashTable *mimetype_map;
	GArray *rules;
};

G_DEFINE_FINAL_TYPE_WITH_CODE (TrackerExtractRulesManager,
                               tracker_extract_rules_manager,
                               G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                      tracker_extract_rules_manager_initable_iface_init))

static void
rule_info_clear (RuleInfo *rule)
{
	g_clear_pointer (&rule->rule_path, g_free);
	g_clear_pointer (&rule->module_path, g_free);
	g_clear_pointer (&rule->graph, g_free);
	g_clear_pointer (&rule->hash, g_free);
	g_clear_pointer (&rule->fallback_rdf_types, g_strfreev);
	g_clear_list (&rule->allow_patterns, (GDestroyNotify) g_pattern_spec_free);
	g_clear_list (&rule->block_patterns, (GDestroyNotify) g_pattern_spec_free);
}

static void
tracker_extract_rules_manager_finalize (GObject *object)
{
	TrackerExtractRulesManager *manager =
		TRACKER_EXTRACT_RULES_MANAGER (object);

	g_clear_pointer (&manager->mimetype_map, g_hash_table_unref);
	g_clear_pointer (&manager->rules, g_array_unref);

	G_OBJECT_CLASS (tracker_extract_rules_manager_parent_class)->finalize (object);
}

static void
tracker_extract_rules_manager_class_init (TrackerExtractRulesManagerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_extract_rules_manager_finalize;
}

static void
tracker_extract_rules_manager_init (TrackerExtractRulesManager *manager)
{
	manager->mimetype_map =
		g_hash_table_new_full (g_str_hash,
		                       g_str_equal,
		                       (GDestroyNotify) g_free,
		                       (GDestroyNotify) g_list_free);

	manager->rules = g_array_new (FALSE, TRUE, sizeof (RuleInfo));
	g_array_set_clear_func (manager->rules, (GDestroyNotify) rule_info_clear);
}

static gboolean
load_extractor_rule (TrackerExtractRulesManager  *manager,
                     GKeyFile                    *key_file,
                     const gchar                 *rule_path,
                     GError                     **error)
{
	GError *local_error = NULL;
	gchar *module_path, **allow_mimetypes, **block_mimetypes;
	gsize n_allow_mimetypes, n_block_mimetypes, i;
	RuleInfo rule = { 0 };

	module_path = g_key_file_get_string (key_file, "ExtractorRule", "ModulePath", &local_error);

	if (local_error) {
		if (!g_error_matches (local_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND)) {
			g_propagate_error (error, local_error);
			return FALSE;
		} else {
			/* Ignore */
			g_clear_error (&local_error);
		}
	}

	if (module_path &&
	    !G_IS_DIR_SEPARATOR (module_path[0])) {
		gchar *tmp;
		const gchar *extractors_dir;

		extractors_dir = g_getenv ("TRACKER_EXTRACTORS_DIR");
		if (G_LIKELY (extractors_dir == NULL)) {
			extractors_dir = TRACKER_EXTRACTORS_DIR;
		}

		tmp = g_build_filename (extractors_dir, module_path, NULL);
		g_free (module_path);
		module_path = tmp;
	}

	allow_mimetypes = g_key_file_get_string_list (key_file, "ExtractorRule", "MimeTypes", &n_allow_mimetypes, &local_error);

	if (!allow_mimetypes) {
		g_free (module_path);

		if (local_error) {
			g_propagate_error (error, local_error);
		}

		return FALSE;
	}

	/* This key is optional */
	block_mimetypes = g_key_file_get_string_list (key_file, "ExtractorRule", "BlockMimeTypes", &n_block_mimetypes, NULL);

	rule.rule_path = g_strdup (rule_path);

	rule.fallback_rdf_types = g_key_file_get_string_list (key_file, "ExtractorRule", "FallbackRdfTypes", NULL, NULL);
	rule.graph = g_key_file_get_string (key_file, "ExtractorRule", "Graph", NULL);
	rule.hash = g_key_file_get_string (key_file, "ExtractorRule", "Hash", NULL);
	rule.module_path = g_strdup (module_path);

	for (i = 0; i < n_allow_mimetypes; i++) {
		GPatternSpec *pattern;

		pattern = g_pattern_spec_new (allow_mimetypes[i]);
		rule.allow_patterns = g_list_prepend (rule.allow_patterns, pattern);
	}

	for (i = 0; i < n_block_mimetypes; i++) {
		GPatternSpec *pattern;

		pattern = g_pattern_spec_new (block_mimetypes[i]);
		rule.block_patterns = g_list_prepend (rule.block_patterns, pattern);
	}

	g_array_append_val (manager->rules, rule);
	g_strfreev (allow_mimetypes);
	g_strfreev (block_mimetypes);
	g_free (module_path);

	return TRUE;
}

gboolean
tracker_extract_rules_manager_initable_init (GInitable     *initable,
                                             GCancellable  *cancellable,
                                             GError       **error)
{
	TrackerExtractRulesManager *manager =
		TRACKER_EXTRACT_RULES_MANAGER (initable);
	const gchar *extractors_dir, *name;
	g_autoptr (GDir) dir = NULL;
	GList *files = NULL, *l;
	GError *inner_error = NULL;

	extractors_dir = g_getenv ("TRACKER_EXTRACTOR_RULES_DIR");
	if (G_LIKELY (extractors_dir == NULL)) {
		extractors_dir = TRACKER_EXTRACTOR_RULES_DIR;
	}

	dir = g_dir_open (extractors_dir, 0, error);
	if (!dir)
		return FALSE;

	while ((name = g_dir_read_name (dir)) != NULL) {
		files = g_list_insert_sorted (files, g_strdup (name),
		                              (GCompareFunc) g_strcmp0);
	}

	TRACKER_NOTE (CONFIG, g_message ("Loading extractor rules... (%s)", extractors_dir));

	for (l = files; l; l = l->next) {
		g_autoptr (GKeyFile) key_file = NULL;
		g_autofree char *path = NULL;
		const gchar *name;

		name = l->data;

		if (!g_str_has_suffix (l->data, ".rule")) {
			TRACKER_NOTE (CONFIG, g_message ("  Skipping file '%s', no '.rule' suffix", name));
			continue;
		}

		path = g_build_filename (extractors_dir, name, NULL);
		key_file = g_key_file_new ();

		if (!g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, error) ||
		    !load_extractor_rule (manager, key_file, path, error))
			break;

		TRACKER_NOTE (CONFIG, g_message ("  Loaded rule '%s'", name));
	}

	g_list_free_full (files, g_free);

	if (inner_error) {
		g_propagate_error (error, inner_error);
		return FALSE;
	}

	TRACKER_NOTE (CONFIG, g_message ("Extractor rules loaded"));

	return TRUE;
}

static void
tracker_extract_rules_manager_initable_iface_init (GInitableIface *iface)
{
	iface->init = tracker_extract_rules_manager_initable_init;
}

TrackerExtractRulesManager *
tracker_extract_rules_manager_new (GError **error)
{
	return g_initable_new (TRACKER_TYPE_EXTRACT_RULES_MANAGER,
	                       NULL, error,
	                       NULL);
}

static gboolean
find_in_patterns (GList      *patterns,
                  const char *mimetype,
                  const char *reversed,
                  int         len)
{
	GList *l;

	for (l = patterns; l; l = l->next) {
#if GLIB_CHECK_VERSION (2, 70, 0)
		if (g_pattern_spec_match (l->data, len, mimetype, reversed))
#else
		if (g_pattern_match (l->data, len, mimetype, reversed))
#endif
		{
			return TRUE;
		}
	}

	return FALSE;
}

static GList *
lookup_rules (TrackerExtractRulesManager *manager,
              const gchar                *mimetype)
{
	g_autofree char *reversed = NULL;
	GList *mimetype_rules = NULL;
	RuleInfo *info;
	gint len, i;

	if (!manager->rules) {
		return NULL;
	}

	if (manager->mimetype_map) {
		mimetype_rules = g_hash_table_lookup (manager->mimetype_map, mimetype);

		if (mimetype_rules) {
			return mimetype_rules;
		}
	}

	reversed = g_strdup (mimetype);
	g_strreverse (reversed);
	len = strlen (mimetype);

	/* Apply the rules! */
	for (i = 0; i < manager->rules->len; i++) {
		gboolean matched_allow_pattern, matched_block_pattern;

		info = &g_array_index (manager->rules, RuleInfo, i);

		matched_allow_pattern = find_in_patterns (info->allow_patterns,
		                                          mimetype, reversed,
		                                          len);
		matched_block_pattern = find_in_patterns (info->block_patterns,
		                                          mimetype, reversed,
		                                          len);

		if (matched_allow_pattern && !matched_block_pattern) {
			mimetype_rules = g_list_prepend (mimetype_rules, info);
		};
	}

	if (mimetype_rules) {
		mimetype_rules = g_list_reverse (mimetype_rules);
		g_hash_table_insert (manager->mimetype_map, g_strdup (mimetype), mimetype_rules);
	}

	return mimetype_rules;
}

GStrv
tracker_extract_rules_manager_get_rdf_types (TrackerExtractRulesManager *manager,
                                             const gchar                *mimetype)
{
	GList *l, *list;
	GHashTable *rdf_types;
	gchar **types, *type;
	GHashTableIter iter;
	gint i;

	list = lookup_rules (manager, mimetype);
	rdf_types = g_hash_table_new (g_str_hash, g_str_equal);

	for (l = list; l; l = l->next) {
		RuleInfo *r_info = l->data;

		if (r_info->fallback_rdf_types == NULL)
			continue;

		for (i = 0; r_info->fallback_rdf_types[i]; i++) {
                        g_debug ("Adding RDF type: %s, for module: %s",
                                 r_info->fallback_rdf_types[i],
                                 r_info->module_path);
			g_hash_table_insert (rdf_types,
					     r_info->fallback_rdf_types[i],
					     r_info->fallback_rdf_types[i]);
		}

                /* We only want the first RDF types matching */
                break;
	}

	g_hash_table_iter_init (&iter, rdf_types);
	types = g_new0 (gchar*, g_hash_table_size (rdf_types) + 1);
	i = 0;

	while (g_hash_table_iter_next (&iter, (gpointer*) &type, NULL)) {
		types[i] = g_strdup (type);
		i++;
	}

	g_hash_table_unref (rdf_types);

	return types;
}

gboolean
tracker_extract_rules_manager_check_fallback_rdf_type (TrackerExtractRulesManager *manager,
                                                       const gchar                *mimetype,
                                                       const gchar                *rdf_type)
{
	GList *l, *list;
	gint i;

	g_return_val_if_fail (mimetype, FALSE);
	g_return_val_if_fail (rdf_type, FALSE);

	list = lookup_rules (manager, mimetype);

	for (l = list; l; l = l->next) {
		RuleInfo *r_info = l->data;

		if (r_info->fallback_rdf_types == NULL)
			continue;

		for (i = 0; r_info->fallback_rdf_types[i]; i++) {
			if (g_strcmp0 (r_info->fallback_rdf_types[i], rdf_type) == 0)
				return TRUE;
		}

                /* We only want the first RDF types matching */
                break;
	}

	return FALSE;
}

const char *
tracker_extract_rules_manager_get_module (TrackerExtractRulesManager *manager,
                                          const gchar                *mimetype)
{
	GList *l, *list;

	g_return_val_if_fail (mimetype != NULL, NULL);

	list = lookup_rules (manager, mimetype);

	for (l = list; l; l = l->next) {
		RuleInfo *r_info = l->data;

		if (r_info->module_path)
			return r_info->module_path;
	}

	return NULL;
}

const gchar *
tracker_extract_rules_manager_get_graph (TrackerExtractRulesManager *manager,
                                         const gchar                *mimetype)
{
	GList *l, *list;

	list = lookup_rules (manager, mimetype);

	for (l = list; l; l = l->next) {
		RuleInfo *r_info = l->data;

		if (r_info->graph)
			return r_info->graph;
	}

	return NULL;
}

const gchar *
tracker_extract_rules_manager_get_hash (TrackerExtractRulesManager *manager,
                                        const gchar                *mimetype)
{
	GList *l, *list;

	list = lookup_rules (manager, mimetype);

	for (l = list; l; l = l->next) {
		RuleInfo *r_info = l->data;

		if (r_info->hash)
			return r_info->hash;
	}

	return NULL;
}
