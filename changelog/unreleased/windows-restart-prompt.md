Enhancement: Prompt for the Windows restart that completes an installation

On the first start of a newly installed or updated version on Windows, the
client now explains that Windows needs to be restarted so that the file
explorer integration (overlay icons, context menu, virtual files sync root)
works correctly, and offers to run the restart right away ("Restart Windows
now" executes the reboot after 10 seconds). The prompt appears once per
installed version. Before rebooting, launch-on-login is enabled so the client
is running again right after the restart.
