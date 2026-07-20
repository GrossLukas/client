Enhancement: Windows setup wrapper that requires and performs the restart

The Windows release now ships an additional Setup installer
(owncloud.online-client-<version>-win-x64-Setup.exe) that stops a running
client first (so the update actually replaces the binaries), installs
silently, and then requires the Windows restart that loads the Explorer
integration - offering to perform it directly. In silent mode (/S, e.g.
baramundi) it reboots automatically unless /norestart is passed.

The in-client restart prompt now also recognizes a recent Windows restart
and skips itself in that case.
