Bugfix: Working, branded Start menu entry on Windows

The embedded base installer creates its Start menu shortcut with the upstream
binary name (bin\owncloud.exe) hardcoded - but this client's binary is
bin\owncloud.online.exe. The resulting "ownCloud" entry pointed at a missing
file: no icon, wrong name, and launching it failed (broken upstream since
client 5.7.x, when packaging moved to the 7z payload/bin layout).

The setup wrapper now repairs this after the installation: it removes the
broken upstream-named shortcuts and creates a working "owncloud.online" Start
menu entry pointing at the real binary, with the branded icon.
