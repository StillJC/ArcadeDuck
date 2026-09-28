# SPDX-FileCopyrightText: 2026 StillJC
# SPDX-License-Identifier: GPL-3.0-only
# Modified for ArcadeDuck by StillJC, 2026.

find_package(Qt6 6.7.2 COMPONENTS Core Gui Widgets LinguistTools REQUIRED)

include(CopyBaseTranslations)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

set(SRCS
  aboutdialog.cpp
  aboutdialog.h
  aboutdialog.ui
  achievementlogindialog.cpp
  achievementlogindialog.h
  achievementlogindialog.ui
  achievementsettingswidget.cpp
  achievementsettingswidget.h
  achievementsettingswidget.ui
  advancedsettingswidget.cpp
  advancedsettingswidget.h
  advancedsettingswidget.ui
  arcadecontrollerbindingwidget.cpp
  arcadecontrollerbindingwidget.h
  audioexpansionsettingsdialog.ui
  audiosettingswidget.cpp
  audiosettingswidget.h
  audiosettingswidget.ui
  audiostretchsettingsdialog.ui
  autoupdaterdialog.cpp
  autoupdaterdialog.h
  autoupdaterdialog.ui
  cheatcodeeditordialog.cpp
  cheatcodeeditordialog.h
  cheatcodeeditordialog.ui
  cheatmanagerwindow.cpp
  cheatmanagerwindow.h
  cheatmanagerwindow.ui
  colorpickerbutton.cpp
  colorpickerbutton.h
  consolesettingswidget.cpp
  consolesettingswidget.h
  consolesettingswidget.ui
  controllerglobalsettingswidget.cpp
  controllerglobalsettingswidget.h
  controllerglobalsettingswidget.ui
  controllerledsettingsdialog.ui
  controllermacroeditwidget.ui
  controllermacrowidget.ui
  controllersettingswindow.cpp
  controllersettingswindow.h
  controllersettingswindow.ui
  controllersettingwidgetbinder.h
  coverdownloaddialog.cpp
  coverdownloaddialog.h
  coverdownloaddialog.ui
  debuggeraddbreakpointdialog.ui
  debuggermodels.cpp
  debuggermodels.h
  debuggerwindow.cpp
  debuggerwindow.h
  debuggerwindow.ui
  displaywidget.cpp
  displaywidget.h
  emptygamelistwidget.ui
  emulationsettingswidget.cpp
  emulationsettingswidget.h
  emulationsettingswidget.ui
  foldersettingswidget.cpp
  foldersettingswidget.h
  foldersettingswidget.ui
  gamelistmodel.cpp
  gamelistmodel.h
  gamelistrefreshthread.cpp
  gamelistrefreshthread.h
  gamelistsettingswidget.cpp
  gamelistsettingswidget.h
  gamelistsettingswidget.ui
  gamelistwidget.cpp
  gamelistwidget.h
  gamelistwidget.ui
  gamesummarywidget.cpp
  gamesummarywidget.h
  gamesummarywidget.ui
  graphicssettingswidget.cpp
  graphicssettingswidget.h
  graphicssettingswidget.ui
  hotkeysettingswidget.cpp
  hotkeysettingswidget.h
  inputbindingdialog.cpp
  inputbindingdialog.h
  inputbindingdialog.ui
  inputbindingwidgets.cpp
  inputbindingwidgets.h
  interfacesettingswidget.cpp
  interfacesettingswidget.h
  interfacesettingswidget.ui
  logwindow.cpp
  logwindow.h
  mainwindow.cpp
  mainwindow.h
  mainwindow.ui
  memoryscannerwindow.cpp
  memoryscannerwindow.h
  memoryscannerwindow.ui
  memoryviewwidget.cpp
  memoryviewwidget.h
  operatorsettingswidget.cpp
  operatorsettingswidget.h
  operatorsettingswidget.ui
  postprocessingsettingswidget.cpp
  postprocessingsettingswidget.h
  postprocessingchainconfigwidget.ui
  reshadepackagemanagerdialog.cpp
  reshadepackagemanagerdialog.h
  qthost.cpp
  qthost.h
  qtkeycodes.cpp
  qtprogresscallback.cpp
  qtprogresscallback.h
  qtthemes.cpp
  qttranslations.cpp
  qtutils.cpp
  qtutils.h
  resource.h
  settingswindow.cpp
  settingswindow.h
  settingswindow.ui
  settingwidgetbinder.h
  systemlinksettingswidget.cpp
  systemlinksettingswidget.h
  setupwizarddialog.cpp
  setupwizarddialog.h
  setupwizarddialog.ui
)

set(TS_FILES
  translations/arcadeduck-qt_de.ts
  translations/arcadeduck-qt_en.ts
  translations/arcadeduck-qt_es.ts
  translations/arcadeduck-qt_fr.ts
  translations/arcadeduck-qt_ja.ts
  translations/arcadeduck-qt_ko.ts
  translations/arcadeduck-qt_pt-BR.ts
  translations/arcadeduck-qt_ru.ts
  translations/arcadeduck-qt_zh-CN.ts
)
add_executable(arcadeduck-qt ${SRCS} ${QM_FILES})
target_precompile_headers(arcadeduck-qt PRIVATE "pch.h")
target_include_directories(arcadeduck-qt PRIVATE "${Qt6Gui_PRIVATE_INCLUDE_DIRS}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(arcadeduck-qt PRIVATE core common imgui minizip scmversion Qt6::Core Qt6::Gui Qt6::Widgets)

# Our Qt builds may have exceptions on, so force them off.
target_compile_definitions(arcadeduck-qt PRIVATE QT_NO_EXCEPTIONS)

add_core_resources(arcadeduck-qt)

if(WIN32)
  target_sources(arcadeduck-qt PRIVATE
    arcadeduck-qt.rc
    vcruntimecheck.cpp
  )

  # We want a Windows subsystem application not console.
  set_target_properties(arcadeduck-qt PROPERTIES
    WIN32_EXECUTABLE TRUE
    OUTPUT_NAME ArcadeDuck
    DEBUG_POSTFIX "-debug")

  # Copy in Qt DLLs. Borrowed from Dolphin.
  get_target_property(MOC_EXECUTABLE_LOCATION Qt6::moc IMPORTED_LOCATION)
  get_filename_component(QT_BINARY_DIRECTORY "${MOC_EXECUTABLE_LOCATION}" DIRECTORY)
  find_program(WINDEPLOYQT_EXE windeployqt HINTS "${QT_BINARY_DIRECTORY}")
  add_custom_command(TARGET arcadeduck-qt POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E env PATH="${QT_BINARY_DIRECTORY}"
            "${WINDEPLOYQT_EXE}" --libdir="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                                 --plugindir="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/QtPlugins"
                                 $<IF:$<CONFIG:Debug>,--debug,--release>
                                 --no-compiler-runtime
                                 --no-system-d3d-compiler
                                 --no-system-dxc-compiler
                                 --no-translations
                                 "$<TARGET_FILE:arcadeduck-qt>"
  )
  add_custom_command(TARGET arcadeduck-qt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/qt.conf.win" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/qt.conf"
  )
  #set_source_files_properties(${TS_FILES} PROPERTIES OUTPUT_LOCATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/translations")

  set(DEPS_TO_COPY cpuinfo.dll discord-rpc.dll freetype.dll harfbuzz.dll libjpeg.dll libpng16.dll libsharpyuv.dll libwebp.dll
                   SDL2.dll shaderc_shared.dll soundtouch.dll spirv-cross-c-shared.dll zlib1.dll zstd.dll)
  foreach(DEP ${DEPS_TO_COPY})
    list(APPEND DEP_BINS "${CMAKE_PREFIX_PATH}/bin/${DEP}")
  endforeach()
  add_custom_command(TARGET arcadeduck-qt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DEP_BINS} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  )
  if(CMAKE_BUILD_TYPE MATCHES "Debug")
    get_property(WINPIXEVENTRUNTIME_DLL TARGET WinPixEventRuntime::WinPixEventRuntime PROPERTY IMPORTED_LOCATION)
    message(STATUS WP "${WINPIXEVENTRUNTIME_DLL}")
    add_custom_command(TARGET arcadeduck-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WINPIXEVENTRUNTIME_DLL}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    )
  endif()
elseif(APPLE)
  # Don't generate a bundle for XCode, it makes code signing fail...
  if(NOT CMAKE_GENERATOR MATCHES "Xcode")
    get_scm_version()
    set(BUNDLE_PATH ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ArcadeDuck.app)
    set_target_properties(arcadeduck-qt PROPERTIES
      MACOSX_BUNDLE true
      MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
      OUTPUT_NAME ArcadeDuck
    )

    # Inject Qt Libraries into bundle.
    if(NOT SKIP_POSTPROCESS_BUNDLE)
      find_program(MACDEPLOYQT_EXE macdeployqt HINTS "${QT_BINARY_DIRECTORY}")
      add_custom_target(arcadeduck-postprocess-bundle ALL
        COMMAND "${MACDEPLOYQT_EXE}" "${BUNDLE_PATH}" -no-strip
      )
      add_dependencies(arcadeduck-postprocess-bundle arcadeduck-qt)
    endif()
  endif()

  # Copy icon into the bundle
  target_sources(arcadeduck-qt PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ArcadeDuck.icns")
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/ArcadeDuck.icns" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

if(ALLOW_INSTALL)
  # Install main binary.
  install(TARGETS arcadeduck-qt RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}")
endif()

# Compile qrc to a binary file.
if(NOT APPLE)
  set(RCC_FILE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/resources/arcadeduck-qt.rcc")
  qt_add_binary_resources(arcadeduck-qt-rcc resources/arcadeduck-qt.qrc DESTINATION ${RCC_FILE} OPTIONS -no-compress)
  add_dependencies(arcadeduck-qt arcadeduck-qt-rcc)
else()
  set(RCC_FILE "${CMAKE_CURRENT_BINARY_DIR}/arcadeduck-qt.rcc")
  qt_add_binary_resources(arcadeduck-qt-rcc resources/arcadeduck-qt.qrc DESTINATION ${RCC_FILE} OPTIONS -no-compress)
  add_dependencies(arcadeduck-qt arcadeduck-qt-rcc)
  target_sources(arcadeduck-qt PRIVATE ${RCC_FILE})
  set_source_files_properties(${RCC_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()


# Translation setup.
qt_add_lrelease(arcadeduck-qt TS_FILES ${TS_FILES} QM_FILES_OUTPUT_VARIABLE QM_FILES)
if(NOT APPLE)
  set(QM_OUTPUT_DIR "$<TARGET_FILE_DIR:arcadeduck-qt>/translations")
  add_custom_command(TARGET arcadeduck-qt POST_BUILD COMMAND "${CMAKE_COMMAND}" -E make_directory "${QM_OUTPUT_DIR}")
  foreach (QM_FILE IN LISTS QM_FILES)
    get_filename_component(QM_FILE_NAME ${QM_FILE} NAME)
    add_custom_command(TARGET arcadeduck-qt POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${QM_FILE}" "${QM_OUTPUT_DIR}/${QM_FILE_NAME}")
    if(ALLOW_INSTALL)
      install(FILES "${QM_FILE}" DESTINATION "${CMAKE_INSTALL_PREFIX}/translations")
    endif()
  endforeach()
else()
  foreach (QM_FILE IN LISTS QM_FILES)
    target_sources(arcadeduck-qt PRIVATE ${QM_FILE})
    set_source_files_properties(${QM_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/translations)
  endforeach()
endif()
copy_base_translations(arcadeduck-qt)
