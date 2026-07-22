Enhancement: Storage quota bar with a nearly-full warning

Each account now shows a slim storage bar directly below its connection
status: used space versus the quota reported by the server, in the
owncloud.online brand color and turning red once usage passes 90%. When
the server reports no limit, the plain used amount is shown instead.

The quota is refreshed when the account connects and every five minutes
while it stays connected. When usage crosses 90% the client shows a
one-time tray warning ("Storage space almost full") so action can be
taken before uploads start failing; the warning re-arms once usage
drops below 85% again.
