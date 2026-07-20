Bugfix: Working, branded Start menu entry on Windows

The embedded base installer creates its Start menu shortcut with the upstream
binary name (bin\owncloud.exe) hardcoded - but this client's binary is
bin\owncloud.online.exe. The resulting "ownCloud" entry pointed at a missing
file: no icon, wrong name, and launching it failed (broken upstream since
client 5.7.x, when packaging moved to the 7z payload/bin layout).

The setup wrapper now repairs this after the installation: it locates the
real client binary (covering the current bin\owncloud.online.exe layout and
older layouts/names), removes the broken upstream-named shortcuts and creates
working "owncloud.online" Start menu AND desktop shortcuts with the branded
icon. The wrapper also gained a directory page and forwards a custom install
path (/D=...) to the embedded installer, which previously ignored it.
