Bugfix: Setup destroyed the installation it had just made (7.8.0-7.10.0)

The embedded craft installer ignores the /D= install directory switch: its
MultiUser initialization unconditionally resets the target directory and
then applies the Install_Dir registry value when one exists. On machines
upgrading from 7.7.x (and on fresh machines) the client was therefore
installed into the old "C:\Program Files\ownCloud" directory - after which
the setup's migration step removed that directory as a supposed leftover,
deleting the freshly installed client. The machine ended up with no client,
dead shortcuts, no Explorer integration and a ghost Apps & Features entry.

The setup now steers the embedded installer via the Install_Dir registry
value (the mechanism it actually honors), verifies that the client binary
really exists in the new directory, and only then removes an old-directory
installation - the migration can no longer touch the installation it just
made. If the client binary cannot be found after a reported success, the
setup says so loudly instead of finishing silently. Running the 7.10.1
setup repairs machines broken by the affected versions.
