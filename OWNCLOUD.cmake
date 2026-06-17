# Modified by BW-Tech GmbH for owncloud.online desktop client branding.
set( APPLICATION_NAME       "owncloud.online" )
set( APPLICATION_SHORTNAME  "owncloud.online" )
set( APPLICATION_EXECUTABLE "owncloud.online" )
set( APPLICATION_DOMAIN     "owncloud.online" )
set( APPLICATION_VENDOR     "BW-Tech GmbH" )
set( APPLICATION_UPDATE_URL "" CACHE STRING "URL for updater" )
set( APPLICATION_ICON_NAME  "owncloud" )
set( APPLICATION_VIRTUALFILE_SUFFIX "owncloud.online" CACHE STRING "Virtual file suffix (not including the .)")

set( LINUX_PACKAGE_SHORTNAME "owncloud-online" )

set( THEME_CLASS            "ownCloudTheme" )
set( APPLICATION_REV_DOMAIN "online.owncloud.desktopclient" )
set( WIN_SETUP_BITMAP_PATH  "${CMAKE_SOURCE_DIR}/admin/win/nsi" )

set( MAC_INSTALLER_BACKGROUND_FILE "${CMAKE_SOURCE_DIR}/admin/osx/installer-background.png" CACHE STRING "The MacOSX installer background image")

set( THEME_INCLUDE          "owncloudtheme.h" )

# set( THEME_INCLUDE          "${OEM_THEME_DIR}/mytheme.h" )
# set( APPLICATION_LICENSE    "${OEM_THEME_DIR}/license.txt )

option( WITH_CRASHREPORTER "Build crashreporter" OFF )
set( CRASHREPORTER_SUBMIT_URL "" CACHE STRING "URL for crash reporter" )

