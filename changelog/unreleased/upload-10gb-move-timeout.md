Bugfix: Uploads of 10 GB and larger no longer time out at the final step

After the last chunk is transferred the server assembles the file in one
long MOVE request during which it sends nothing - for very large files
this takes well over the default 5-minute network timeout. The timeout
is scaled with the file size (3 minutes per gigabyte), but the scaling
had a boundary bug: at 10 GB the computed value hit the 30-minute cap
and was then silently discarded instead of clamped, so files of exactly
10 GB or more kept the 5-minute default and the upload failed at 100%.

The cap now clamps (up to 60 minutes), so the client waits long enough
for the server to finish assembling files of 10 GB and beyond.
