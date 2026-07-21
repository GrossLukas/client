Bugfix: Explorer integration (overlay icons, context menu) is now activated

The installer shipped the Explorer shell extensions (OCOverlays.dll and
OCContextMenu.dll) but never registered them with Windows - so Explorer
showed neither the sync status overlay icons (green check, blue sync,
red error) nor the owncloud.online context menu (Share, Show in web
browser, Copy private link, conflict actions, Free up space / Always
keep on this device).

The setup wrapper now registers both extensions during installation.
After the (already required) Windows restart, sync folders show live
status badges on every file and folder, and the right-click menu offers
the full owncloud.online action set.
