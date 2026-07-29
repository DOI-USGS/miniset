# MinisetApps.cmake — helper for the miniset app paradigm.
#
# Native miniset apps use ISIS-agnostic, snake_case names (e.g. image_import).
# For users familiar with ISIS, an app may also expose one or more ISIS-style
# compatibility aliases (e.g. isisimport). An alias is the SAME binary installed
# under another name; the program adapts its usage/messages to argv[0]. Aliases
# are created as symlinks so there is a single implementation to maintain.
#
# Usage:
#   miniset_add_app(image_import
#       SOURCES  apps/image_import.cpp
#       LINK     miniset
#       INCLUDE  ${CMAKE_CURRENT_SOURCE_DIR}
#       ALIASES  isisimport)          # optional ISIS-compat names

function(miniset_add_app app_name)
    set(options)
    set(one_value_args)
    set(multi_value_args SOURCES LINK INCLUDE ALIASES)
    cmake_parse_arguments(APP "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    add_executable(${app_name} ${APP_SOURCES})
    if(APP_LINK)
        target_link_libraries(${app_name} PRIVATE ${APP_LINK})
    endif()
    if(APP_INCLUDE)
        target_include_directories(${app_name} PRIVATE ${APP_INCLUDE})
    endif()
    install(TARGETS ${app_name} RUNTIME DESTINATION bin)

    # ISIS-compat aliases: symlinks to the native binary, in build tree + install.
    foreach(alias ${APP_ALIASES})
        add_custom_command(TARGET ${app_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                    $<TARGET_FILE_NAME:${app_name}>
                    $<TARGET_FILE_DIR:${app_name}>/${alias}
            COMMENT "Creating ISIS-compat alias ${alias} -> ${app_name}")
        install(CODE "\
            execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink \
                ${app_name} \
                \"\${CMAKE_INSTALL_PREFIX}/bin/${alias}\")")
    endforeach()
endfunction()
