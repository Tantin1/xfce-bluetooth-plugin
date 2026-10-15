cmake_minimum_required(VERSION 3.16)
project(xfce-bluetooth-plugin C)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install prefix" FORCE)
endif()

find_package(PkgConfig REQUIRED)
find_package(Intl REQUIRED)

pkg_check_modules(GTK3       REQUIRED gtk+-3.0>=3.24)
pkg_check_modules(XFCE4PANEL REQUIRED libxfce4panel-2.0>=4.18)
pkg_check_modules(GIO        REQUIRED gio-2.0>=2.56)
pkg_check_modules(LIBNOTIFY           libnotify>=0.7)

set(GETTEXT_PACKAGE "xfce-bluetooth-plugin")
set(LOCALE_DIR "${CMAKE_INSTALL_PREFIX}/share/locale")

add_definitions(
    -DGETTEXT_PACKAGE="${GETTEXT_PACKAGE}"
    -DLOCALEDIR="${LOCALE_DIR}"
)
if(LIBNOTIFY_FOUND)
    add_definitions(-DHAVE_LIBNOTIFY)
    message(STATUS "libnotify: encontrada, notificaciones activadas")
else()
    message(STATUS "libnotify: NO encontrada, notificaciones desactivadas")
endif()

add_library(xfce-bluetooth-plugin MODULE
    plugin.c
    popup.c
    popup-rows.c
    popup-sections.c
    bt-dbus.c
    bt-agent.c
    bt-obex.c
    bt-obex-agent.c
)

target_include_directories(xfce-bluetooth-plugin PRIVATE
    ${GTK3_INCLUDE_DIRS}
    ${XFCE4PANEL_INCLUDE_DIRS}
    ${GIO_INCLUDE_DIRS}
    ${LIBNOTIFY_INCLUDE_DIRS}
)

target_compile_options(xfce-bluetooth-plugin PRIVATE
    ${GTK3_CFLAGS_OTHER}
    ${XFCE4PANEL_CFLAGS_OTHER}
    -Wall -Wextra
)

target_link_libraries(xfce-bluetooth-plugin PRIVATE
    ${GTK3_LIBRARIES}
    ${XFCE4PANEL_LIBRARIES}
    ${GIO_LIBRARIES}
    ${Intl_LIBRARIES}
    ${LIBNOTIFY_LIBRARIES}
)

if(NOT XFCE_PLUGIN_DIR)
    if(EXISTS "/usr/lib/x86_64-linux-gnu/xfce4/panel/plugins")
        set(XFCE_PLUGIN_DIR "/usr/lib/x86_64-linux-gnu/xfce4/panel/plugins")
    else()
        set(XFCE_PLUGIN_DIR "/usr/lib/xfce4/panel/plugins")
    endif()
endif()
message(STATUS "Plugin install dir: ${XFCE_PLUGIN_DIR}")

install(FILES "${CMAKE_BINARY_DIR}/libxfce-bluetooth-plugin.so"
    DESTINATION "${XFCE_PLUGIN_DIR}"
)

# Compilar e instalar traducciones.
# La lista de idiomas se lee de po/LINGUAS, el mismo archivo que usa Meson:
# así hay una sola lista para los dos sistemas. Se admiten varios idiomas por
# línea y comentarios con «#». Si LINGUAS cambia, CMake se reconfigura solo.
file(STRINGS "${CMAKE_SOURCE_DIR}/po/LINGUAS" _linguas_lines ENCODING UTF-8)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/po/LINGUAS")
set(LANGS "")
foreach(_line ${_linguas_lines})
    string(REGEX REPLACE "#.*$" "" _line "${_line}")
    string(STRIP "${_line}" _line)
    if(_line)
        string(REGEX REPLACE "[ \t]+" ";" _line "${_line}")
        list(APPEND LANGS ${_line})
    endif()
endforeach()
message(STATUS "Traducciones: ${LANGS}")

foreach(LANG ${LANGS})
    set(PO_FILE "${CMAKE_SOURCE_DIR}/po/${LANG}.po")
    set(MO_FILE "${CMAKE_BINARY_DIR}/po/${LANG}.mo")
    add_custom_command(
        OUTPUT  "${MO_FILE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/po"
        COMMAND msgfmt "${PO_FILE}" -o "${MO_FILE}"
        DEPENDS "${PO_FILE}"
    )
    list(APPEND MO_FILES "${MO_FILE}")
    install(FILES "${MO_FILE}"
        DESTINATION "${CMAKE_INSTALL_PREFIX}/share/locale/${LANG}/LC_MESSAGES"
        RENAME "xfce-bluetooth-plugin.mo"
    )
endforeach()
add_custom_target(translations ALL DEPENDS ${MO_FILES})

# El .desktop.in trae @plugin_dir@ (el nombre que usa Meson). Definimos esa
# misma variable acá para que los dos sistemas de compilación compartan el
# mismo archivo de entrada sin tocarlo.
set(plugin_dir "${XFCE_PLUGIN_DIR}")

configure_file(
    "${CMAKE_SOURCE_DIR}/xfce-bluetooth-plugin.desktop.in"
    "${CMAKE_BINARY_DIR}/xfce-bluetooth-plugin.desktop"
    @ONLY
)

install(FILES "${CMAKE_BINARY_DIR}/xfce-bluetooth-plugin.desktop"
    DESTINATION "${CMAKE_INSTALL_PREFIX}/share/xfce4/panel/plugins"
)
