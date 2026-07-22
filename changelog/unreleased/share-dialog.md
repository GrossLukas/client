Enhancement: Share files and folders directly from the client

Right-clicking a synced file or folder in Explorer and choosing "Share
with owncloud.online" now opens a native sharing dialog instead of the
web browser. It lists the existing public links (copy to clipboard,
delete) and creates new ones - optionally protected with a password
and/or an expiration date - via the server's OCS share API. A freshly
created link is copied to the clipboard automatically. Server-side
policies (e.g. an enforced password) are reported with the server's
own message. When the share API is disabled on the server, the
browser share page opens as before.
