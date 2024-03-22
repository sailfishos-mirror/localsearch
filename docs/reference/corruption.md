# Handling of corrupted databases

The database offered by Tracker Miners is a fast access cache of filesystem
data. In the unlikely event of major database corruption (e.g. filesystem-level
corruption), this data can be disposed and reproduceably rebuilt with no major
loss other than the CPU and I/O time spent.

Still, the preferred action is to attempt database repairs first. The expected
order of events is:

1. `tracker-miner-fs-3` receives an unexpected `TRACKER_SPARQL_ERROR_CORRUPTED`
   error from the Tracker SPARQL library.
2. The `tracker-miner-fs-3` will handle this error by exiting uncleanly.
3. The process gets restarted.
4. When the database is re-opened, the Tracker SPARQL library will detect the
   prior corruption, and run repair attempts. See the
   [related library documentation](https://gnome.pages.gitlab.gnome.org/tracker/ctor.SparqlConnection.new.html)
   for more details.
5. If the repair attempt failed, the corrupted database file(s) will be moved
   aside at `~/.cache/tracker3/files.$TIMESTAMP`, and the filesystem data will
   be reindexed from scratch.
