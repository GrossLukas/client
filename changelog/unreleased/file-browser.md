Enhancement: Browse the remote file structure directly in the folder list

The folder rows on the account page are now expandable: expanding a row
lists the remote subfolders (loaded lazily from the server, with their
sizes), and those rows can be expanded further, so the whole directory
tree can be browsed without leaving the client.

For folders synced without virtual files the rows carry the selective
sync checkboxes known from the classic client; changes are applied via
an apply/discard bar below the list. For virtual files folders the rows
show the current availability instead, and a right click sets the pin
state (always keep on this device / free up space / reset to default).
