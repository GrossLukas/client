Bugfix: Large uploads no longer fail on server-side transaction deadlocks

Uploading large files (chunked upload) could permanently fail against
owncloud.online (oc10) servers on MySQL/MariaDB with "SQLSTATE[40001]:
Serialization failure: 1213 Deadlock found when trying to get lock" - the
concurrent chunk PUTs into the same upload directory provoke the deadlock.

The client now recognizes this reply as transient: the affected chunk (or the
MKCOL / final assembling MOVE) is retried in the same sync run with a backoff,
and the transfer - as well as all further chunked uploads of the sync run -
falls back to serial chunk uploads to stop provoking the deadlock. The new
OWNCLOUD_MAX_PARALLEL_CHUNKS environment variable can pin the chunk
concurrency (e.g. to 1) for affected servers.
