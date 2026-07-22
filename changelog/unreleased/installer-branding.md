Enhancement: Installer defaults to C:\Program Files\owncloud.online and brands the uninstall entry

The Windows setup now installs to "C:\Program Files\owncloud.online" by
default (a different path can still be chosen on the directory page or
via /D=...). An existing installation in the old default directory
"C:\Program Files\ownCloud" is uninstalled first so upgrades move over
cleanly - accounts and sync folders live in the user profile and are
not affected.

The entry in Windows' "Apps & Features" is rebranded as well: it now
reads "owncloud.online Client <version>" with publisher "BW.Tech GmbH"
and the branded icon, instead of the upstream "ownCloud" / "ownCloud
GmbH" naming.
