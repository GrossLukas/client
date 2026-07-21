Enhancement: Sync folders are pinned to the Explorer navigation pane

Classic sync folders now appear as their own entry in the left-hand
navigation pane of Windows Explorer - with the owncloud.online icon,
the way OneDrive appears there. One entry is created per sync folder
(named after the folder when several are configured), giving one-click
access to the synced data from every Explorer window and file dialog.

Virtual-files folders already got their entry from the Windows Cloud
Files API registration; this covers the classic (non-VFS) folders. The
pins live in the current user's registry (no admin rights involved),
follow folder additions, removals and renames automatically, and can be
turned off in Settings > General with "Show sync folders in the Explorer
navigation pane".
