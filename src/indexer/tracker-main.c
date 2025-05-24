/*
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>

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

#include <string.h>
#include <stdlib.h>
#include <locale.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <unistd.h>

#ifdef HAVE_MALLOC_TRIM
#include <malloc.h>
#endif

#include <glib.h>
#include <glib-unix.h>
#include <glib-object.h>
#include <glib/gi18n.h>

#include <tracker-common.h>

#include "tracker-config.h"
#include "tracker-controller.h"
#include "tracker-miner-files.h"
#include "tracker-files-interface.h"

#include <tinysparql.h>

#define ABOUT	  \
	"Tracker " PACKAGE_VERSION "\n"

#define LICENSE	  \
	"This program is free software and comes without any warranty.\n" \
	"It is licensed under version 2 or later of the General Public " \
	"License which can be viewed at:\n" \
	"\n" \
	"  http://www.gnu.org/licenses/gpl.txt\n"

#define DBUS_NAME_SUFFIX "LocalSearch3"
#define LEGACY_DBUS_NAME_SUFFIX "Tracker3.Miner.Files"
#define DBUS_PATH "/org/freedesktop/Tracker3/Miner/Files"

static GMainLoop *main_loop;
static guint cleanup_id;

static gint initial_sleep = -1;
static gboolean no_daemon;
static gchar *eligible;
static gboolean version;
static guint miners_timeout_id = 0;
static gboolean dry_run = FALSE;

static gboolean slept = TRUE;
static gboolean graphs_ready = FALSE;
static gboolean corrupted = FALSE;

static GOptionEntry entries[] = {
	{ "initial-sleep", 's', 0,
	  G_OPTION_ARG_INT, &initial_sleep,
	  N_("Initial sleep time in seconds, "
	     "0->1000 (default=15)"),
	  NULL },
	{ "no-daemon", 'n', 0,
	  G_OPTION_ARG_NONE, &no_daemon,
	  N_("Runs until all configured locations are indexed and then exits"),
	  NULL },
	{ "eligible", 'e', 0,
	  G_OPTION_ARG_FILENAME, &eligible,
	  N_("Checks if FILE is eligible for being mined based on configuration"),
	  N_("FILE") },
	{ "dry-run", 'r', 0,
	  G_OPTION_ARG_NONE, &dry_run,
	  N_("Avoids changes in the filesystem"),
	  NULL },
	{ "version", 'V', 0,
	  G_OPTION_ARG_NONE, &version,
	  N_("Displays version information"),
	  NULL },
	{ NULL }
};

typedef struct {
	TrackerSparqlConnection *sparql_conn;
	GDBusConnection *dbus_conn;
	GMainLoop *main_loop;
	GMutex mutex;
	GCond cond;
	GThread *thread;
	gboolean initialized;
	GError *error;
} EndpointThreadData;

static EndpointThreadData *endpoint_thread_data = NULL;

static void
log_option_values (TrackerConfig *config)
{
#ifdef G_ENABLE_DEBUG
	if (TRACKER_DEBUG_CHECK (CONFIG)) {
		g_message ("General options:");
		g_message ("  Initial Sleep  ........................  %d",
		           initial_sleep);
	}
#endif
}

static GFile *
get_cache_dir (void)
{
	GFile *cache;

	if (MINER_FS_CACHE_LOCATION[0] == G_DIR_SEPARATOR) {
		cache = g_file_new_for_path (MINER_FS_CACHE_LOCATION);
	} else {
		g_autofree char *cache_dir = NULL;

		cache_dir = g_build_filename (g_get_user_cache_dir (),
		                              MINER_FS_CACHE_LOCATION,
		                              "files", NULL);
		cache = g_file_new_for_path (cache_dir);
	}

	return cache;
}

static gboolean
signal_handler (gpointer user_data)
{
	int signo = GPOINTER_TO_INT (user_data);

	static gboolean in_loop = FALSE;

	/* Die if we get re-entrant signals handler calls */
	if (in_loop) {
		_exit (EXIT_FAILURE);
	}

	switch (signo) {
	case SIGTERM:
	case SIGINT:
		in_loop = TRUE;
		g_main_loop_quit (main_loop);

		/* Fall through */
	default:
		if (g_strsignal (signo)) {
			g_debug ("Received signal:%d->'%s'",
			         signo,
			         g_strsignal (signo));
		}
		break;
	}

	return G_SOURCE_CONTINUE;
}

static void
initialize_signal_handler (void)
{
#ifndef G_OS_WIN32
	g_unix_signal_add (SIGTERM, signal_handler, GINT_TO_POINTER (SIGTERM));
	g_unix_signal_add (SIGINT, signal_handler, GINT_TO_POINTER (SIGINT));
#endif /* G_OS_WIN32 */
}

static void
initialize_priority_and_scheduling (void)
{
	/* Set CPU priority */
	tracker_sched_idle ();

	/* Set disk IO priority and scheduling */
	tracker_ioprio_init ();

	/* Set process priority:
	 * The nice() function uses attribute "warn_unused_result" and
	 * so complains if we do not check its returned value. But it
	 * seems that since glibc 2.2.4, nice() can return -1 on a
	 * successful call so we have to check value of errno too.
	 * Stupid...
	 */

	TRACKER_NOTE (CONFIG, g_message ("Setting priority nice level to 19"));

	errno = 0;
	if (nice (19) == -1 && errno != 0) {
		const gchar *str = g_strerror (errno);

		g_message ("Couldn't set nice value to 19, %s",
		           str ? str : "no error given");
	}
}

static void
raise_file_descriptor_limit (void)
{
	struct rlimit rl;

	if (getrlimit (RLIMIT_NOFILE, &rl) != 0)
		return;

	rl.rlim_cur = rl.rlim_max;
	if (setrlimit(RLIMIT_NOFILE, &rl) != 0)
		g_warning ("Failed to increase file descriptor limit: %m");
}

static void
miner_do_start (TrackerMiner *miner)
{
	if (!tracker_miner_is_started (miner)) {
		g_debug ("Starting filesystem miner...");
		tracker_miner_start (miner);
	}
}

static void
miner_maybe_start (TrackerMiner *miner)
{
	if (!slept || !graphs_ready)
		return;

	miner_do_start (miner);
}

static gboolean
miner_start_idle_cb (gpointer data)
{
	TrackerMiner *miner = data;

	miners_timeout_id = 0;
	slept = TRUE;
	miner_maybe_start (miner);
	return G_SOURCE_REMOVE;
}

static void
miner_start (TrackerMiner *miner)
{
	/* If requesting to run as no-daemon, start right away */
	if (no_daemon) {
		miner_maybe_start (miner);
		return;
	}

	/* If no need to initially sleep, start right away */
	if (initial_sleep <= 0) {
		miner_maybe_start (miner);
		return;
	}

	slept = FALSE;
	g_debug ("Performing initial sleep of %d seconds",
	         initial_sleep);
	miners_timeout_id = g_timeout_add_seconds (initial_sleep,
	                                           miner_start_idle_cb,
	                                           miner);
}

static void
release_heap_memory (void)
{
#ifdef HAVE_MALLOC_TRIM
	malloc_trim (0);
#else
	g_debug ("release_heap_memory(): Doing nothing as malloc_trim() is not available on this platform.");
#endif
}

static gboolean
cleanup_cb (gpointer user_data)
{
	release_heap_memory ();

	cleanup_id = 0;

	return G_SOURCE_REMOVE;
}

static void
start_cleanup_timeout (void)
{
	if (cleanup_id == 0)
		cleanup_id = g_timeout_add_seconds (30, cleanup_cb, NULL);
}

static void
stop_cleanup_timeout (void)
{
	g_clear_handle_id (&cleanup_id, g_source_remove);
}

#if GLIB_CHECK_VERSION (2, 64, 0)
static void
on_low_memory (GMemoryMonitor            *monitor,
               GMemoryMonitorWarningLevel level,
               gpointer                   user_data)
{
	if (level > G_MEMORY_MONITOR_WARNING_LEVEL_LOW)
		release_heap_memory ();
}
#endif

static void
miner_started_cb (TrackerMinerFS *fs)
{
	stop_cleanup_timeout ();
}

static void
miner_finished_cb (TrackerMinerFS *fs,
                   gpointer        user_data)
{
	/* We're not sticking around for file updates, so stop
	 * the mainloop and exit.
	 */
	if (no_daemon && main_loop) {
		/* FIXME: wait for extractor to finish */
		g_main_loop_quit (main_loop);
	}
}

static void
miner_status_cb (TrackerMinerFS *fs)
{
	g_autofree gchar *status = NULL;

	g_object_get (G_OBJECT (fs), "status", &status, NULL);
	if (g_strcmp0 (status, "Idle") == 0)
		start_cleanup_timeout ();
	else
		stop_cleanup_timeout ();
}

static void
miner_corrupt_cb (TrackerMinerFS *fs)
{
	g_warning ("Database corruption detected, bailing out");
	corrupted = TRUE;
	g_main_loop_quit (main_loop);
}

static void
graphs_created_cb (GObject      *source,
                   GAsyncResult *res,
                   gpointer      user_data)
{
	TrackerMiner *miner = user_data;

	tracker_sparql_connection_update_finish (TRACKER_SPARQL_CONNECTION (source),
	                                         res, NULL);
	graphs_ready = TRUE;
	miner_maybe_start (miner);
}

static gint
check_eligible (TrackerIndexingTree *indexing_tree,
                TrackerStorage      *storage)
{
	g_autoptr (TrackerController) controller = NULL;
	g_autoptr (GFile) file = NULL;
	g_autoptr (GFileInfo) info = NULL;
	g_autoptr (GError) error = NULL;
	g_autofree gchar *path = NULL;
	gboolean exists = TRUE;
	gboolean indexable;
	gboolean parents_indexable = TRUE;
	gboolean is_dir;

	controller = tracker_controller_new (indexing_tree, storage, NULL);

	/* Start check */
	file = g_file_new_for_commandline_arg (eligible);
	info = g_file_query_info (file,
	                          G_FILE_ATTRIBUTE_STANDARD_TYPE ","
	                          G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
	                          G_FILE_QUERY_INFO_NONE,
	                          NULL,
	                          &error);

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
		exists = FALSE;

	if (info) {
		is_dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
	} else {
		/* Assume not a dir */
		is_dir = FALSE;
	}

	path = g_file_get_path (file);

	g_print (exists ?
	         _("Data object “%s” currently exists") :
	         _("Data object “%s” currently does not exist"),
	         path);

	g_print ("\n");

	indexable = tracker_indexing_tree_file_is_indexable (indexing_tree, file, info);

	if (!indexable) {
		if (is_dir &&
		    tracker_indexing_tree_file_matches_filter (indexing_tree, TRACKER_FILTER_DIRECTORY, file)) {
			g_print ("  %s\n", _("Directory is NOT eligible to be indexed (based on filters)"));

		} else if (!is_dir &&
		           tracker_indexing_tree_file_matches_filter (indexing_tree, TRACKER_FILTER_FILE, file)) {
			g_print ("  %s\n", _("File is NOT eligible to be indexed (based on filters)"));
		} else if (tracker_file_is_hidden (file) &&
		           tracker_indexing_tree_get_filter_hidden (indexing_tree)) {
			g_print ("  %s\n", _("File is NOT eligible to be indexed (hidden file)"));
		} else {
			g_print ("  %s\n", _("File is NOT eligible to be indexed (not an indexed folder)"));
		}
	}

	if (indexable) {
		GFile *root, *parent;
		GList *files = NULL, *l;

		root = tracker_indexing_tree_get_root (indexing_tree, file, NULL, NULL);
		parent = file;

		/* Still, a parent folder might be filtered out, figure it out */
		while (parent && !g_file_equal (parent, root)) {
			parent = g_file_get_parent (parent);
			files = g_list_prepend (files, parent);
		}

		for (l = files; l; l = l->next) {
			g_autofree gchar *dir_path = NULL;

			dir_path = g_file_get_path (l->data);

			if (is_dir &&
			    tracker_indexing_tree_file_matches_filter (indexing_tree, TRACKER_FILTER_DIRECTORY, l->data)) {
				g_print (_("Parent directory “%s” is NOT eligible to be indexed (based on filters)"),
				         dir_path);
				g_print ("\n");
				parents_indexable = FALSE;
			} else if (tracker_file_is_hidden (l->data) &&
			           tracker_indexing_tree_get_filter_hidden (indexing_tree)) {
				g_print (_("Parent directory “%s” is NOT eligible to be indexed (hidden file)"),
				         dir_path);
				g_print ("\n");
				parents_indexable = FALSE;
			} else {
				if (!tracker_indexing_tree_parent_is_indexable (indexing_tree, l->data)) {
					g_print (_("Parent directory “%s” is NOT eligible to be indexed (based on content filters)"),
					         dir_path);
					g_print ("\n");
					parents_indexable = FALSE;
				}
			}

			if (!parents_indexable)
				break;
		}

		g_list_free_full (files, g_object_unref);
	}

	if (indexable && parents_indexable) {
		g_print ("  %s\n",
		         is_dir ?
		         _("Directory is eligible to be indexed") :
		         _("File is eligible to be indexed"));
	}

	return (indexable && parents_indexable) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void
on_domain_vanished (GDBusConnection *connection,
                    const gchar     *name,
                    gpointer         user_data)
{
	GMainLoop *loop = user_data;
	g_message ("Domain %s vanished: quitting now.", name);
	g_main_loop_quit (loop);
}

gpointer
endpoint_thread_func (gpointer user_data)
{
	EndpointThreadData *data = user_data;
	TrackerEndpointDBus *endpoint;
	GMainContext *main_context;

	main_context = g_main_context_new ();
	g_main_context_push_thread_default (main_context);

	endpoint = tracker_endpoint_dbus_new (data->sparql_conn,
	                                      data->dbus_conn,
	                                      NULL,
	                                      NULL,
	                                      &data->error);

	data->main_loop = g_main_loop_new (main_context, FALSE);

	g_mutex_lock (&data->mutex);
	data->initialized = TRUE;
	g_cond_signal (&data->cond);
	g_mutex_unlock (&data->mutex);

	if (!data->error)
		g_main_loop_run (data->main_loop);

	g_main_context_pop_thread_default (main_context);
	g_main_loop_unref (data->main_loop);
	g_main_context_unref (main_context);
	g_clear_object (&endpoint);

	return NULL;
}

gboolean
start_endpoint_thread (TrackerSparqlConnection  *conn,
                       GDBusConnection          *dbus_conn,
                       GError                  **error)
{
	g_assert (endpoint_thread_data == NULL);

	endpoint_thread_data = g_new0 (EndpointThreadData, 1);
	g_mutex_init (&endpoint_thread_data->mutex);
	g_cond_init (&endpoint_thread_data->cond);
	endpoint_thread_data->sparql_conn = conn;
	endpoint_thread_data->dbus_conn = dbus_conn;

	endpoint_thread_data->thread =
		g_thread_try_new ("SPARQL endpoint",
		                  endpoint_thread_func,
		                  endpoint_thread_data,
		                  error);

	if (!endpoint_thread_data->thread)
		return FALSE;

	g_mutex_lock (&endpoint_thread_data->mutex);

	while (!endpoint_thread_data->initialized) {
		g_cond_wait (&endpoint_thread_data->cond,
		             &endpoint_thread_data->mutex);
	}

	g_mutex_unlock (&endpoint_thread_data->mutex);

	if (endpoint_thread_data->error) {
		g_propagate_error (error, endpoint_thread_data->error);
		g_thread_join (endpoint_thread_data->thread);
		g_clear_pointer (&endpoint_thread_data, g_free);
		return FALSE;
	}

	return TRUE;
}

void
finish_endpoint_thread (void)
{
	g_assert (endpoint_thread_data != NULL);

	g_main_loop_quit (endpoint_thread_data->main_loop);

	g_thread_join (endpoint_thread_data->thread);

	g_clear_pointer (&endpoint_thread_data, g_free);
}

static TrackerSparqlConnection *
setup_connection (GError **error)
{
	TrackerSparqlConnection *sparql_conn;
	g_autoptr (GFile) store = NULL, ontology = NULL;
	GError *internal_error = NULL;
	TrackerSparqlConnectionFlags flags =
		TRACKER_SPARQL_CONNECTION_FLAGS_FTS_ENABLE_STEMMER |
		TRACKER_SPARQL_CONNECTION_FLAGS_FTS_ENABLE_UNACCENT;

	if (!dry_run)
		store = get_cache_dir ();
	ontology = tracker_sparql_get_ontology_nepomuk ();

	sparql_conn = tracker_sparql_connection_new (flags,
	                                             store,
	                                             ontology,
	                                             NULL,
	                                             &internal_error);

	if (store && g_error_matches (internal_error, TRACKER_SPARQL_ERROR, TRACKER_SPARQL_ERROR_CORRUPT)) {
		g_autoptr (GFile) backup_location = NULL, parent = NULL;
		g_autofree gchar *filename = NULL;

		/* Move the database directory away, for possible forensics */
		parent = g_file_get_parent (store);
		filename = g_strdup_printf ("files.%" G_GINT64_FORMAT, g_get_monotonic_time ());
		backup_location = g_file_get_child (parent, filename);

		if (g_file_move (store, backup_location,
		                 G_FILE_COPY_NONE,
		                 NULL, NULL, NULL, NULL)) {
			g_autofree gchar *path = NULL;

			path = g_file_get_path (backup_location);
			g_message ("Database is corrupt, it is now backed up at %s. Reindexing from scratch", path);
			sparql_conn = tracker_sparql_connection_new (flags,
			                                             store,
			                                             ontology,
			                                             NULL,
			                                             &internal_error);
		}
	}

	if (internal_error)
		g_propagate_error (error, internal_error);

	return sparql_conn;
}

int
main (gint argc, gchar *argv[])
{
	TrackerConfig *config;
	TrackerMiner *miner_files;
	GOptionContext *context;
	GError *error = NULL;
	TrackerMinerProxy *proxy;
	GDBusConnection *connection;
	TrackerSparqlConnection *sparql_conn;
	TrackerIndexingTree *indexing_tree;
	TrackerStorage *storage;
	TrackerController *controller;
#if GLIB_CHECK_VERSION (2, 64, 0)
	GMemoryMonitor *memory_monitor;
#endif
	g_autofree char *dbus_name = NULL, *legacy_dbus_name = NULL;
	TrackerFilesInterface *files_interface;
	gboolean initial_index = TRUE;

	main_loop = NULL;

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	/* Set timezone info */
	tzset ();

	/* This makes sure we don't steal all the system's resources */
	initialize_priority_and_scheduling ();

	/* This makes it harder to run out of file descriptors while there
	 * are many concurrently running queries through the endpoint.
	 */
	raise_file_descriptor_limit ();

	/* Translators: this messagge will apper immediately after the
	 * usage string - Usage: COMMAND <THIS_MESSAGE>
	 */
	context = g_option_context_new (_("— start the tracker indexer"));

	g_option_context_add_main_entries (context, entries, NULL);
	g_option_context_parse (context, &argc, &argv, &error);
	g_option_context_free (context);

	if (error) {
		g_printerr ("%s\n", error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	if (version) {
		g_print ("\n" ABOUT "\n" LICENSE "\n");
		return EXIT_SUCCESS;
	}

	indexing_tree = tracker_indexing_tree_new ();
	storage = tracker_storage_new ();

	if (eligible) {
		return check_eligible (indexing_tree, storage);
	}

	connection = g_bus_get_sync (TRACKER_IPC_BUS, NULL, &error);
	if (error) {
		g_critical ("Could not create DBus connection: %s\n",
		            error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	files_interface = tracker_files_interface_new (connection);

	/* Initialize logging */
	config = tracker_config_new ();

	if (initial_sleep < 0)
		initial_sleep = g_settings_get_int (G_SETTINGS (config), "initial-sleep");

	log_option_values (config);

	main_loop = g_main_loop_new (NULL, FALSE);

	if (no_daemon) {
		g_debug ("tracker-miner-fs-3 running in --no-daemon mode.");
	} else {
		g_debug ("tracker-miner-fs-3 running as " DOMAIN_PREFIX "." DBUS_NAME_SUFFIX);

		if (g_strcmp0 (DOMAIN_PREFIX, "org.freedesktop") != 0) {
			g_debug ("tracker-miner-fs-3 running for domain " DOMAIN_PREFIX ". "
			         "The service will exit when " DOMAIN_PREFIX " disappears from the bus.");

			g_bus_watch_name_on_connection (connection, DOMAIN_PREFIX,
			                                G_BUS_NAME_WATCHER_FLAGS_NONE,
			                                NULL, on_domain_vanished,
			                                main_loop, NULL);
		}
	}

	if (!dry_run) {
		GFile *store = get_cache_dir ();

		initial_index = !g_file_query_exists (store, NULL);
		tracker_error_report_init (store);
		g_object_unref (store);
	}

	sparql_conn = setup_connection (&error);
	if (!sparql_conn) {

		g_critical ("Could not create store: %s",
		            error->message);
		g_error_free (error);

		return EXIT_FAILURE;
	}

	if (!start_endpoint_thread (sparql_conn, connection, &error)) {
		g_critical ("Could not set up SPARQL endpoint: %s",
		            error->message);
		g_error_free (error);

		return EXIT_FAILURE;
	}

	/* Create new TrackerMinerFiles object */
	miner_files = tracker_miner_files_new (sparql_conn,
	                                       indexing_tree,
	                                       storage,
	                                       config,
	                                       initial_index);

	controller = tracker_controller_new (indexing_tree, storage, files_interface);

	proxy = tracker_miner_proxy_new (miner_files, connection, DBUS_PATH, NULL, &error);
	if (error) {
		g_critical ("Couldn't create miner proxy: %s", error->message);
		g_error_free (error);
		g_object_unref (config);
		g_object_unref (miner_files);
		return EXIT_FAILURE;
	}

	/* Request DBus names */
	dbus_name = g_strconcat (DOMAIN_PREFIX, ".", DBUS_NAME_SUFFIX, NULL);

	if (!tracker_dbus_request_name (connection, dbus_name, &error)) {
		g_critical ("Could not request DBus name '%s': %s",
		            dbus_name, error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	legacy_dbus_name = g_strconcat (DOMAIN_PREFIX, ".", LEGACY_DBUS_NAME_SUFFIX, NULL);

	if (!tracker_dbus_request_name (connection, legacy_dbus_name, &error)) {
		g_critical ("Could not request legacy DBus name '%s': %s",
		            legacy_dbus_name, error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	g_signal_connect (miner_files, "started",
			  G_CALLBACK (miner_started_cb),
			  NULL);
	g_signal_connect (miner_files, "finished",
			  G_CALLBACK (miner_finished_cb),
			  NULL);
	g_signal_connect (miner_files, "notify::status",
	                  G_CALLBACK (miner_status_cb),
	                  NULL);
	g_signal_connect (miner_files, "corrupt",
			  G_CALLBACK (miner_corrupt_cb),
			  NULL);

#if GLIB_CHECK_VERSION (2, 64, 0)
	memory_monitor = g_memory_monitor_dup_default ();
	g_signal_connect (memory_monitor, "low-memory-warning", G_CALLBACK (on_low_memory), NULL);
#endif

	/* Preempt creation of graphs */
	tracker_sparql_connection_update_async (tracker_miner_get_connection (miner_files),
	                                        "CREATE SILENT GRAPH tracker:FileSystem; "
	                                        "CREATE SILENT GRAPH tracker:Software; "
	                                        "CREATE SILENT GRAPH tracker:Documents; "
	                                        "CREATE SILENT GRAPH tracker:Pictures; "
	                                        "CREATE SILENT GRAPH tracker:Audio; "
	                                        "CREATE SILENT GRAPH tracker:Video ",
	                                        NULL, graphs_created_cb, miner_files);

	miner_start (miner_files);

	initialize_signal_handler ();

	/* Go, go, go! */
	g_main_loop_run (main_loop);

	g_debug ("Shutdown started");

	finish_endpoint_thread ();

	g_object_unref (files_interface);

	g_main_loop_unref (main_loop);
	g_object_unref (config);

	g_object_unref (controller);

	g_object_unref (miner_files);

	g_object_unref (proxy);
	g_object_unref (connection);

	tracker_sparql_connection_close (sparql_conn);
	g_object_unref (sparql_conn);

	g_clear_object (&indexing_tree);
	g_clear_object (&storage);

#if GLIB_CHECK_VERSION (2, 64, 0)
	g_signal_handlers_disconnect_by_func (memory_monitor, on_low_memory, NULL);
	g_object_unref (memory_monitor);
#endif

	return corrupted ? EXIT_FAILURE : EXIT_SUCCESS;
}
