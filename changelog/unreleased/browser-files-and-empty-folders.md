Enhancement: The folder browser shows files and handles empty folders properly

Expanding a folder in the in-app folder browser now also lists the
files it contains (with their size, or their availability when virtual
files are on), not only the subfolders. Files are informational rows
below the folders; syncing selection stays folder-based. With virtual
files on, the right-click pin menu (Always keep on this device / Free
up space) works on single files too.

A folder that is empty on the server now says so ("This folder is empty
on the server.") instead of leaving a stale "Loading ..." row behind,
and keeps its expander like Explorer does - collapsing and re-expanding
re-checks the server. A folder that vanished from the server gets a
clearer message as well.

The pointless "0 out of 0 folders are synchronized" line below the
folder list was removed.
