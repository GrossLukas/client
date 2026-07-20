Bugfix: Large downloads survive interruptions and keep going

A dropped connection, stall or server hiccup during a large download no
longer fails the file - and a timeout no longer aborts the whole sync run
(it was classified as a fatal error before). The transfer now resumes
in-run from the partial temp file with a short backoff; every attempt that
received data resets the retry counter, so a download that keeps making
progress is resumed indefinitely. Only repeated attempts with zero progress
hand over to the normal error handling - and even then the retry backoff
for resumable downloads is capped at one minute (instead of growing to
24 hours), so the next sync run picks the file up right where it stopped.
Net effect: a 10 GB download keeps inching forward until it completes.
