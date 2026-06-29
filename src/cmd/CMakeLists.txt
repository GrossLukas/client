add_executable(owncloudcmd
    cmd.cpp
    httpbasiccredentials.cpp
)
set_target_properties(owncloudcmd PROPERTIES OUTPUT_NAME "owncloudcmd")
ecm_mark_nongui_executable(owncloudcmd)

target_link_libraries(owncloudcmd csync libsync Qt::Core Qt::Network)
apply_common_target_settings(owncloudcmd)

if(NOT APPLE)
    install(TARGETS owncloudcmd ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
endif()
