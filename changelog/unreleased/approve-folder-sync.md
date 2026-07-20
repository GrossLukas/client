Enhancement: Bring back the folder-sync approval options

The general settings offer the two classic approval options again (only
effective while virtual files are off): "Ask for confirmation before
synchronizing folders larger than X MB" (default 500 MB) and "Ask for
confirmation before synchronizing external storages".

When a new remote folder exceeds the limit or comes from an external storage
mount, it is excluded from the sync, announced via a notification, and the
client asks directly whether it should be synchronized to this computer.
Approved folders sync immediately; declined folders stay excluded (they can be
re-included any time via the folder browser checkboxes or "Manage subfolder
sync").
