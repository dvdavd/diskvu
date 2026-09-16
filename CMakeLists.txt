cmake_minimum_required(VERSION 3.21)
project(diskvu VERSION 0.9.27 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(DISKVU_PORTABLE_X86_64 "Build x86-64 binaries for the widest CPU compatibility." OFF)
option(DISKVU_COVERAGE "Build test targets with gcov coverage instrumentation." OFF)

configure_file(src/version.h.in src/version.h @ONLY)

include(GNUInstallDirs)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent Svg LinguistTools)
find_package(Qt6 QUIET COMPONENTS DBus)

qt_standard_project_setup()

if (DISKVU_PORTABLE_X86_64 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    add_compile_options(-march=x86-64 -mtune=generic)
endif()

if (WIN32)
    set(SCANNER_BACKEND_SRC src/scanner_win32.cpp)
else()
    set(SCANNER_BACKEND_SRC src/scanner_posix.cpp)
endif()

if (WIN32)
    set(THUMBNAIL_SRC src/thumbnailprovider_win32.cpp)
elseif (APPLE)
    set(THUMBNAIL_SRC src/thumbnailprovider_macos.mm)
else()
    set(THUMBNAIL_SRC src/thumbnailprovider_posix.cpp)
endif()

qt_add_executable(diskvu
    src/main.cpp
    src/filesystemutils.h
    src/filesystemutils.cpp
    src/scanner.h
    src/scanner.cpp
    ${SCANNER_BACKEND_SRC}
    src/thumbnailprovider.h
    ${THUMBNAIL_SRC}
    src/filenode.h
    src/iconutils.h
    src/svgutils.h
    src/svgutils.cpp
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
    src/filterparams.h
    src/searchfilterpanel.h
    src/searchfilterpanel.cpp
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

qt_add_translations(diskvu
    TS_FILES
        translations/diskvu_en_US.ts
    LUPDATE_OPTIONS
        -no-obsolete
    LRELEASE_OPTIONS
        -compress
)

target_include_directories(diskvu PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/src)

target_link_libraries(diskvu PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Svg
)

if(WIN32)
    set_target_properties(diskvu PROPERTIES WIN32_EXECUTABLE TRUE)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/diskvu.ico")
        configure_file(
            packaging/windows/diskvu.rc.in
            diskvu.rc
            @ONLY
        )
        target_sources(diskvu PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/diskvu.rc)
    endif()

    configure_file(
        packaging/windows/installer.nsi.in
        installer.nsi
        @ONLY
    )
endif()

if(APPLE)
    set_target_properties(diskvu PROPERTIES
        OUTPUT_NAME "Diskvu"
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Diskvu"
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.dvdavd.Diskvu"
        MACOSX_BUNDLE_COPYRIGHT "© 2026 dvdavd"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in"
    )

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/diskvu.icns")
        target_sources(diskvu PRIVATE packaging/macos/diskvu.icns)
        set_source_files_properties(packaging/macos/diskvu.icns
            PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(diskvu PROPERTIES
            MACOSX_BUNDLE_ICON_FILE "diskvu.icns")
    endif()
endif()

if (TARGET Qt6::DBus)
    target_link_libraries(diskvu PRIVATE Qt6::DBus)
    target_compile_definitions(diskvu PRIVATE DISKVU_HAS_QT_DBUS)
endif()

if (APPLE)
    target_link_libraries(diskvu PRIVATE
        "-framework QuickLookThumbnailing"
        "-framework CoreGraphics")
endif()

if (WIN32)
    target_link_libraries(diskvu PRIVATE shell32 ole32 kernel32)
endif()

qt_add_executable(scanner_bench EXCLUDE_FROM_ALL
    src/scanner_bench.cpp
    src/filesystemutils.h
    src/filesystemutils.cpp
    src/scanner.h
    src/scanner.cpp
    ${SCANNER_BACKEND_SRC}
    src/colorutils.h
    src/colorutils.cpp
    src/treemapsettings.h
    src/treemapsettings.cpp
)

target_include_directories(scanner_bench PRIVATE src)

target_link_libraries(scanner_bench PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

if (WIN32)
    target_link_libraries(scanner_bench PRIVATE kernel32)
endif()

install(TARGETS diskvu
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/${PROJECT_NAME}
)

if(UNIX AND NOT APPLE)
    install(FILES assets/diskvu_appicon.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME io.github.dvdavd.Diskvu.svg
    )

    configure_file(
        packaging/io.github.dvdavd.Diskvu.desktop.in
        io.github.dvdavd.Diskvu.desktop
        @ONLY
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.dvdavd.Diskvu.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )

    install(FILES packaging/io.github.dvdavd.Diskvu.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo
    )
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────

if(BUILD_TESTING)
find_package(Qt6 REQUIRED COMPONENTS Test)
enable_testing()

function(diskvu_add_test target)
    qt_add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/src)
    target_link_libraries(${target} PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test Qt6::Concurrent Qt6::Svg)

    if (TARGET Qt6::DBus)
        target_link_libraries(${target} PRIVATE Qt6::DBus)
        target_compile_definitions(${target} PRIVATE DISKVU_HAS_QT_DBUS)
    endif()

    if (WIN32)
        target_link_libraries(${target} PRIVATE shell32 ole32 kernel32)
    endif()

    if (DISKVU_COVERAGE)
        target_compile_options(${target} PRIVATE --coverage)
        target_link_options(${target} PRIVATE --coverage)
    endif()

    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

set(DISKVU_SETTINGS_SRC
    src/treemapsettings.h
    src/treemapsettings.cpp
)

set(DISKVU_COLORUTILS_SRC
    src/colorutils.h
    src/colorutils.cpp
)

set(DISKVU_FILESYSTEMUTILS_SRC
    src/filesystemutils.h
    src/filesystemutils.cpp
)

set(DISKVU_WIDGET_SRC
    ${DISKVU_FILESYSTEMUTILS_SRC}
    src/filenode.h
    src/svgutils.h
    src/svgutils.cpp
    src/mainwindow_utils.h
    src/mainwindow_utils.cpp
    ${DISKVU_SETTINGS_SRC}
    ${DISKVU_COLORUTILS_SRC}
    src/thumbnailprovider.h
    ${THUMBNAIL_SRC}
    src/treemapwidget.h
    src/treemapwidget.cpp
    src/treemap_drawing.h
    src/treemap_drawing.cpp
)

diskvu_add_test(test_settings
    tests/test_settings.cpp
    ${DISKVU_FILESYSTEMUTILS_SRC}
    ${DISKVU_SETTINGS_SRC}
)

diskvu_add_test(test_drawing
    tests/test_drawing.cpp
    ${DISKVU_FILESYSTEMUTILS_SRC}
    src/filenode.h
    ${DISKVU_SETTINGS_SRC}
    src/treemap_drawing.h
    src/treemap_drawing.cpp
    ${DISKVU_COLORUTILS_SRC}
    src/svgutils.h
    src/svgutils.cpp
)

diskvu_add_test(test_filenode
    tests/test_filenode.cpp
    ${DISKVU_WIDGET_SRC}
)

diskvu_add_test(test_icons
    tests/test_icons.cpp
    ${DISKVU_WIDGET_SRC}
)

diskvu_add_test(test_scanner
    tests/test_scanner.cpp
    ${DISKVU_FILESYSTEMUTILS_SRC}
    src/scanner.h
    src/scanner.cpp
    ${SCANNER_BACKEND_SRC}
    ${DISKVU_SETTINGS_SRC}
    ${DISKVU_COLORUTILS_SRC}
)

diskvu_add_test(test_search
    tests/test_search.cpp
    ${DISKVU_WIDGET_SRC}
)

diskvu_add_test(test_refresh
    tests/test_refresh.cpp
    src/scanner.h
    src/scanner.cpp
    ${SCANNER_BACKEND_SRC}
    ${DISKVU_WIDGET_SRC}
)

diskvu_add_test(test_breadcrumbpathbar
    tests/test_breadcrumbpathbar.cpp
    src/breadcrumbpathbar.h
    src/breadcrumbpathbar.cpp
    src/iconutils.h
    ${DISKVU_WIDGET_SRC}
)

diskvu_add_test(test_searchfilterpanel
    tests/test_searchfilterpanel.cpp
    src/searchfilterpanel.h
    src/searchfilterpanel.cpp
    src/iconutils.h
    ${DISKVU_WIDGET_SRC}
)

diskvu_add_test(test_settingsdialog
    tests/test_settingsdialog.cpp
    src/settingsdialog.h
    src/settingsdialog.cpp
    resources.qrc
    ${DISKVU_WIDGET_SRC}
)

diskvu_add_test(test_filesystemutils
    tests/test_filesystemutils.cpp
    ${DISKVU_FILESYSTEMUTILS_SRC}
)

diskvu_add_test(test_filesystemwatchcontroller
    tests/test_filesystemwatchcontroller.cpp
    src/filesystemwatchcontroller.h
    src/filesystemwatchcontroller.cpp
    src/scanner.h
    src/scanner.cpp
    ${SCANNER_BACKEND_SRC}
    ${DISKVU_WIDGET_SRC}
)

diskvu_add_test(test_subtree_refresh_paths
    tests/test_subtree_refresh_paths.cpp
    src/scanner.h
    src/scanner.cpp
    ${SCANNER_BACKEND_SRC}
    ${DISKVU_WIDGET_SRC}
)

endif() # BUILD_TESTING

configure_file(cmake/uninstall.cmake.in cmake/uninstall.cmake @ONLY)

add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -E echo "Uninstalling..."
    COMMAND ${CMAKE_COMMAND} -DINSTALL_MANIFEST="${CMAKE_BINARY_DIR}/install_manifest.txt" -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/uninstall.cmake"
    COMMENT "Uninstalling diskvu"
)
