function(copy_base_translations target)
  get_target_property(MOC_EXECUTABLE_LOCATION Qt6::moc IMPORTED_LOCATION)
  get_filename_component(QT_BINARY_DIRECTORY "${MOC_EXECUTABLE_LOCATION}" DIRECTORY)
  find_program(LCONVERT_EXE lconvert HINTS "${QT_BINARY_DIRECTORY}")
  set(BASE_TRANSLATIONS_DIR "${QT_BINARY_DIRECTORY}/../translations")

  if(NOT APPLE)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/translations")
  endif()

  # Keep this synchronized with Host::GetAvailableLanguageList(). English does
  # not require a Qt base translation because Qt source strings are English.
  set(BASE_TRANSLATION_LANGUAGES
    de
    es
    fr
    ja
    ko
    pt_BR
    ru
    zh_CN
  )

  foreach(lang IN LISTS BASE_TRANSLATION_LANGUAGES)
    set(path "${BASE_TRANSLATIONS_DIR}/qt_${lang}.qm")
    set(baseQmPath "${BASE_TRANSLATIONS_DIR}/qtbase_${lang}.qm")

    # If qtbase_<lang>.qm exists, merge the Qt module translations for that
    # language into the single qt_<lang>.qm file ArcadeDuck loads at runtime.
    if(EXISTS "${baseQmPath}")
      set(outPath "${CMAKE_CURRENT_BINARY_DIR}/qt_${lang}.qm")
      set(srcQmFiles)
      file(GLOB langQmFiles "${BASE_TRANSLATIONS_DIR}/qt*${lang}.qm")
      foreach(qmFile IN LISTS langQmFiles)
        get_filename_component(file ${qmFile} NAME)
        if(file STREQUAL "qt_${lang}.qm")
          continue()
        endif()
        list(APPEND srcQmFiles "${qmFile}")
      endforeach()

      if(srcQmFiles)
        add_custom_command(OUTPUT ${outPath}
          COMMAND "${LCONVERT_EXE}" -verbose -of qm -o "${outPath}" ${srcQmFiles}
          DEPENDS ${srcQmFiles}
        )
        set(path "${outPath}")
      endif()
    endif()

    if(NOT EXISTS "${path}" AND NOT path MATCHES "^${CMAKE_CURRENT_BINARY_DIR}")
      message(STATUS "Qt base translation not available for ${lang}; skipping")
      continue()
    endif()

    target_sources(${target} PRIVATE ${path})
    if(APPLE)
      set_source_files_properties(${path} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/translations)
    else()
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${path}" "$<TARGET_FILE_DIR:${target}>/translations")
    endif()
  endforeach()
endfunction()