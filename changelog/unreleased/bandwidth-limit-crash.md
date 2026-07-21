Bugfix: Enabling a bandwidth limit no longer crashes the client

With an upload or download speed limit active, the bandwidth manager
wakes transfers up through the event queue and hands out quota once a
second. Such a wakeup could arrive just after a transfer had finished
and its network reply was already gone - an internal assertion then
terminated the client immediately (the sync itself continued fine after
a restart, which is why files still arrived).

The wakeup handler now simply ignores transfers whose reply is gone,
so bandwidth limiting works while large syncs are running.
