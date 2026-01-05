# Indexed data

## Folders

By default, LocalSearch indexes the user home directory recursively. In order to
reduce the amount of superfluous data in the LocalSearch index, certain folders
will be skipped in treatment. Not only, but most notably:

- Mountpoints expressed in `/etc/fstab`.
- Hidden folders
- Git repositories
- Directories with a `.nomedia` file

The `ignored-directories` and `ignored-directories-with-content` settings may be
used to partially change this behavior. Hidden folders and mountpoints are
invariably ignored, unless configured themselves as an indexed folder.

## Files

Files are generally indexed to make them searchable by name and filesystem
characteristics. With a few exceptions:

- Hidden files
- A wide variety of backup files

The `ignored-files` setting may be used to partially change this behavior.
Hidden files are invariably ignored.

## File content

LocalSearch has a number of metadata extractors for a wide variety of popular
formats:

- Video
- Audio
- Documents
- Playlists
- Games

These metadata extractors will collect information from the content of files
based on their detected mimetype, and make this information available in the
index. This metadata will be stored in a structured manner, expressed by the
[Nepomuk ontology](https://gnome.pages.gitlab.gnome.org/tinysparql/docs/developer/ontologies.html#nepomuk).
