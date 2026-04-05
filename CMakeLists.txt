cmake_minimum_required(VERSION 3.21)
project(diskscape VERSION 0.9.21 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(DISKSCAPE_PORTABLE_X86_64 "Build x86-64 binaries for the widest CPU compatibility." OFF)
option(DISKSCAPE_COVERAGE "Build test targets with gcov coverage instrumentation." OFF)

configure_file(src/version.h.in src/version.h @ONLY)

include(GNUInstallDirs)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent Svg LinguistTools)
find_package(Qt6 QUIET COMPONENTS DBus)

qt_standard_project_setup()

if (DISKSCAPE_PORTABLE_X86_64 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    add_compile_options(-march=x86-64 -mtune=generic)
endif()

if (WIN32)
    set(SCANNER_BACKEND_SRC src/scanner_win32.cpp)
else()
    set(SCANNER_BACKEND_SRC src/scanner_posix.cpp)
endif()

qt_add_executable(diskscape
    src/main.cpp
    src/scanner.h
    ${SCANNER_BACKEND_SRC}
    src/filenode.h
    src/iconutils.h
    src/colorutils.h
    src/colorutils.cpp
    src/treemapsettings.h
    src/treemapsettings.cpp
    src/mainwindow_types.h
    src/treemap_drawing.h
    src/treemap_drawing.cpp
    src/treemapwidget.h
    src/treemapwidget.cpp
    src/breadcrumbpathbar.h
    src/breadcrumbpathbar.cpp
    src/filesystemwatchcontroller.h
    src/filesystemwatchcontroller.cpp
    src/settingsdialog.h
    src/settingsdialog.cpp
    src/mainwindow.h
    src/mainwindow_utils.h
    src/mainwindow_utils.cpp
    src/platformthemewatcher.h
    src/platformthemewatcher.cpp
    src/mainwindow_landing.cpp
    src/mainwindow.cpp
    src/nodepropertiesdialog.h
    src/nodepropertiesdialog.cpp
    resources.qrc
)

qt_add_translations(diskscape
    TS_FILES
        translations/diskscape_en_US.ts
    LUPDATE_OPTIONS
        -no-obsolete
    LRELEASE_OPTIONS
        -compress
)

target_include_directories(diskscape PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/src)

target_link_libraries(diskscape PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Svg
)

if(APPLE)
    set_target_properties(diskscape PROPERTIES OUTPUT_NAME "Diskscape")
endif()

if (TARGET Qt6::DBus)
    target_link_libraries(diskscape PRIVATE Qt6::DBus)
    target_compile_definitions(diskscape PRIVATE DISKSCAPE_HAS_QT_DBUS)
endif()

qt_add_executable(scanner_bench EXCLUDE_FROM_ALL
    src/scanner_bench.cpp
    src/scanner.h
    ${SCANNER_BACKEND_SRC}
    src/colorutils.h
    src/colorutils.cpp
    src/treemapsettings.h
    src/treemapsettings.cpp
)

target_include_directories(scanner_bench PRIVATE src)

target_link_libraries(scanner_bench PRIVATE
    Qt6::Core
)

install(TARGETS diskscape
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/${PROJECT_NAME}
)

if(UNIX AND NOT APPLE)
    install(FILES assets/diskscape.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME io.github.dvdavd.Diskscape.svg
    )

    configure_file(
        packaging/io.github.dvdavd.Diskscape.desktop.in
        io.github.dvdavd.Diskscape.desktop
        @ONLY
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.dvdavd.Diskscape.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )

    install(FILES packaging/io.github.dvdavd.Diskscape.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo
    )
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────

if(BUILD_TESTING)
find_package(Qt6 REQUIRED COMPONENTS Test)
enable_testing()

function(diskscape_add_test target)
    qt_add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/src)
    target_link_libraries(${target} PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test Qt6::Concurrent Qt6::Svg)

    if (TARGET Qt6::DBus)
        target_link_libraries(${target} PRIVATE Qt6::DBus)
        target_compile_definitions(${target} PRIVATE DISKSCAPE_HAS_QT_DBUS)
    endif()

    if (DISKSCAPE_COVERAGE)
        target_compile_options(${target} PRIVATE --coverage)
        target_link_options(${target} PRIVATE --coverage)
    endif()

    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

set(DISKSCAPE_SETTINGS_SRC
    src/treemapsettings.h
    src/treemapsettings.cpp
)

set(DISKSCAPE_COLORUTILS_SRC
    src/colorutils.h
    src/colorutils.cpp
)

diskscape_add_test(test_settings
    tests/test_settings.cpp
    ${DISKSCAPE_SETTINGS_SRC}
)

diskscape_add_test(test_drawing
    tests/test_drawing.cpp
    src/filenode.h
    ${DISKSCAPE_SETTINGS_SRC}
    src/treemap_drawing.h
    src/treemap_drawing.cpp
    ${DISKSCAPE_COLORUTILS_SRC}
)

diskscape_add_test(test_filenode
    tests/test_filenode.cpp
    src/filenode.h
    src/mainwindow_utils.h
    src/mainwindow_utils.cpp
    ${DISKSCAPE_SETTINGS_SRC}
    ${DISKSCAPE_COLORUTILS_SRC}
    src/treemapwidget.h
    src/treemapwidget.cpp
    src/treemap_drawing.h
    src/treemap_drawing.cpp
)

diskscape_add_test(test_icons
    tests/test_icons.cpp
    src/mainwindow_utils.h
    src/mainwindow_utils.cpp
    ${DISKSCAPE_SETTINGS_SRC}
    ${DISKSCAPE_COLORUTILS_SRC}
    src/treemapwidget.h
    src/treemapwidget.cpp
    src/treemap_drawing.h
    src/treemap_drawing.cpp
)

diskscape_add_test(test_scanner
    tests/test_scanner.cpp
    src/scanner.h
    ${SCANNER_BACKEND_SRC}
    ${DISKSCAPE_SETTINGS_SRC}
    ${DISKSCAPE_COLORUTILS_SRC}
)

set(DISKSCAPE_WIDGET_SRC
    src/filenode.h
    src/mainwindow_utils.h
    src/mainwindow_utils.cpp
    ${DISKSCAPE_SETTINGS_SRC}
    ${DISKSCAPE_COLORUTILS_SRC}
    src/treemapwidget.h
    src/treemapwidget.cpp
    src/treemap_drawing.h
    src/treemap_drawing.cpp
)

diskscape_add_test(test_search
    tests/test_search.cpp
    ${DISKSCAPE_WIDGET_SRC}
)

diskscape_add_test(test_refresh
    tests/test_refresh.cpp
    src/scanner.h
    ${SCANNER_BACKEND_SRC}
    ${DISKSCAPE_WIDGET_SRC}
)

diskscape_add_test(test_breadcrumbpathbar
    tests/test_breadcrumbpathbar.cpp
    src/breadcrumbpathbar.h
    src/breadcrumbpathbar.cpp
    src/iconutils.h
    ${DISKSCAPE_WIDGET_SRC}
)

endif() # BUILD_TESTING

configure_file(cmake/uninstall.cmake.in cmake/uninstall.cmake @ONLY)

add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -E echo "Uninstalling..."
    COMMAND ${CMAKE_COMMAND} -DINSTALL_MANIFEST="${CMAKE_BINARY_DIR}/install_manifest.txt" -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/uninstall.cmake"
    COMMENT "Uninstalling diskscape"
)
