# SPDX-FileCopyrightText: 2026 StillJC
# SPDX-License-Identifier: GPL-3.0-only
# Modified for ArcadeDuck by StillJC, 2026.

add_library(core
  achievements.cpp
  achievements.h
  arcade/arcade_control_registry.cpp
  arcade/arcade_control_registry.h
  arcade/arcade_database.cpp
  arcade/arcade_database.h
  arcade/arcade_input.cpp
  arcade/arcade_input.h
  arcade/arcade_machine_handler.cpp
  arcade/arcade_machine_handler.h
  arcade/arcade_output.cpp
  arcade/arcade_output.h
  arcade/arcade_output_network.cpp
  arcade/arcade_output_network.h
  arcade/arcade_output_win32.cpp
  arcade/arcade_output_win32.h
  achievements_private.h
  bios.cpp
  bios.h
  bus.cpp
  bus.h
  cdrom.cpp
  cdrom.h
  cdrom_async_reader.cpp
  cdrom_async_reader.h
  cheats.cpp
  cheats.h
  cpu_code_cache.cpp
  cpu_code_cache.h
  cpu_code_cache_private.h
  cpu_core.cpp
  cpu_core.h
  cpu_core_private.h
  cpu_disasm.cpp
  cpu_disasm.h
  cpu_pgxp.cpp
  cpu_pgxp.h
  cpu_types.cpp
  cpu_types.h
  dma.cpp
  dma.h
  fullscreen_ui.cpp
  fullscreen_ui.h
  game_database.cpp
  game_database.h
  game_list.cpp
  game_list.h
  gdb_server.cpp
  gdb_server.h
  gpu.cpp
  gpu.h
  gpu_backend.cpp
  gpu_backend.h
  gpu_commands.cpp
  gpu_hw.cpp
  gpu_hw.h
  gpu_hw_shadergen.cpp
  gpu_hw_shadergen.h
  gpu_shadergen.cpp
  gpu_shadergen.h
  gpu_sw.cpp
  gpu_sw.h
  gpu_sw_backend.cpp
  gpu_sw_backend.h
  gpu_types.h
  gte.cpp
  gte.h
  gte_types.h
  host.cpp
  host.h
  host_interface_progress_callback.cpp
  host_interface_progress_callback.h
  hotkeys.cpp
  input_types.h
  imgui_overlays.cpp
  imgui_overlays.h
  interrupt_controller.cpp
  interrupt_controller.h
  arcade/devices/audio/k054539.cpp
  arcade/devices/audio/k054539.h
  arcade/devices/audio/ymf271.cpp
  arcade/devices/audio/ymf271.h
  arcade/devices/security/cat702.cpp
  arcade/devices/storage/chd_hard_disk.cpp
  arcade/devices/storage/chd_hard_disk.h
  arcade/devices/storage/ncr53cf96.cpp
  arcade/devices/security/cat702.h
  arcade/third_party/qsound_hle/qsound.cpp
  arcade/third_party/qsound_hle/qsound.h
  arcade/third_party/ymfm/ymfm.h
  arcade/third_party/ymfm/ymfm_adpcm.cpp
  arcade/third_party/ymfm/ymfm_adpcm.h
  arcade/third_party/ymfm/ymfm_fm.h
  arcade/third_party/ymfm/ymfm_fm.ipp
  arcade/third_party/ymfm/ymfm_opn.cpp
  arcade/third_party/ymfm/ymfm_opn.h
  arcade/third_party/ymfm/ymfm_ssg.cpp
  arcade/third_party/ymfm/ymfm_ssg.h
  arcade/third_party/z80_superzazu/z80.cpp
  arcade/third_party/z80_superzazu/z80.h
  arcade/systems/sony/zn/acclaim_ata.cpp
  arcade/systems/sony/zn/acclaim_ata.h
  arcade/systems/sony/zn/time_warner_ata.cpp
  arcade/systems/sony/zn/time_warner_ata.h
  arcade/systems/sony/zn/acclaim_rax.cpp
  arcade/systems/sony/zn/acclaim_rax.h
  arcade/third_party/adsp2181/adsp2181_core.c
  arcade/third_party/adsp2181/adsp2181_core.h
  arcade/third_party/adsp2181/2100ops.inc
  arcade/systems/sony/zn/capcom_qsound.cpp
  arcade/systems/sony/zn/capcom_qsound.h
  arcade/systems/sony/zn/atlus_zn1_sound.cpp
  arcade/systems/sony/zn/atlus_zn1_sound.h
  arcade/systems/sony/zn/eighting_raizing_sound.cpp
  arcade/systems/sony/zn/eighting_raizing_sound.h
  arcade/systems/sony/zn/tecmo_cbaj_sound.cpp
  arcade/systems/sony/zn/tecmo_cbaj_sound.h
  arcade/systems/sony/zn/tecmo_gr2_link.cpp
  arcade/systems/sony/zn/tecmo_gr2_link.h
  arcade/systems/sony/zn/taito_gnet_comm.cpp
  arcade/systems/sony/zn/taito_gnet_comm.h
  arcade/systems/sony/zn/ymz280b.cpp
  arcade/systems/sony/zn/ymz280b.h
  arcade/systems/sony/zn/taito_fx1a_sound.cpp
  arcade/systems/sony/zn/taito_fx1a_sound.h
  arcade/systems/sony/zn/taito_fx1b_mn10200.cpp
  arcade/systems/sony/zn/taito_fx1b_mn10200.h
  arcade/systems/sony/zn/taito_fx1b_zsg2.cpp
  arcade/systems/sony/zn/taito_fx1b_zsg2.h
  arcade/systems/sony/zn/taito_fx1b_zoom.cpp
  arcade/systems/sony/zn/taito_fx1b_zoom.h
  arcade/systems/sony/zn/sony_zn.cpp
  arcade/systems/sony/zn/sony_zn.h
  arcade/devices/storage/ncr53cf96.h
  arcade/systems/konami/konami.cpp
  arcade/systems/konami/konami.h
  arcade/systems/konami/gq/konami_gq_eeprom.cpp
  arcade/systems/konami/gq/konami_gq_eeprom.h
  arcade/systems/konami/gq/konami_gq_hdd.cpp
  arcade/systems/konami/gq/konami_gq_hdd.h
  arcade/systems/konami/gq/konami_gq_k056800.cpp
  arcade/systems/konami/gq/konami_gq_k056800.h
  arcade/systems/konami/gq/konami_gq_pcm.cpp
  arcade/systems/konami/gq/konami_gq_pcm.h
  arcade/systems/konami/gq/konami_gq_scsi.cpp
  arcade/systems/konami/gq/konami_gq_scsi.h
  arcade/systems/konami/gq/konami_gq_sound_cpu.cpp
  arcade/systems/konami/gq/konami_gq_sound_cpu.h
  arcade/systems/konami/gq/konami_gq_tms57002.cpp
  arcade/systems/konami/gq/konami_gq_tms57002.h
  arcade/systems/konami/gq/konami_gq_tms57002.hxx
  arcade/systems/konami/gq/konami_gq_tms57002_ops.cpp
  m37710/m37710.cpp
  m37710/m37710.h
  m37710/m37710cm.h
  m37710/m37710il.h
  m37710/m37710o0.cpp
  m37710/m37710o1.cpp
  m37710/m37710o2.cpp
  m37710/m37710o3.cpp
  m37710/m37710op.h
  musashi/m68k.h
  musashi/m68k_bus.cpp
  musashi/m68k_bus.h
  musashi/m68kconf.h
  musashi/m68kcpu.c
  musashi/m68kcpu.h
  musashi/m68kops.c
  musashi/m68kops.h
  arcade/systems/konami/gv/konami_gv_cdrom.cpp
  arcade/systems/konami/gv/konami_gv_cdrom.h
  arcade/systems/konami/gv/konami_gv_scsi.cpp
  arcade/systems/konami/gv/konami_gv_scsi.h
  arcade/systems/namco/system11/namco_system11.cpp
  arcade/systems/namco/system11/namco_system11.h
  legacy_pad_state.cpp
  legacy_pad_state.h
  mdec.cpp
  mdec.h
  mips_encoder.h
  pad.h
  pcdrv.cpp
  pcdrv.h
  pine_server.cpp
  pine_server.h
  save_state_version.h
  settings.cpp
  settings.h
  shader_cache_version.h
  sio.cpp
  sio.h
  spu.cpp
  spu.h
  system.cpp
  system.h
  texture_replacements.cpp
  texture_replacements.h
  timers.cpp
  timers.h
  timing_event.cpp
  timing_event.h
  types.h
)

set(RECOMPILER_SRCS
  cpu_recompiler_code_generator.cpp
  cpu_recompiler_code_generator.h
  cpu_recompiler_code_generator_generic.cpp
  cpu_recompiler_register_cache.cpp
  cpu_recompiler_register_cache.h
  cpu_recompiler_thunks.h
  cpu_recompiler_types.h
)

set(NEWREC_SOURCES
  cpu_newrec_compiler.cpp
  cpu_newrec_compiler.h
)

target_precompile_headers(core PRIVATE "pch.h")
set_property(TARGET core PROPERTY C_STANDARD 11)
set_source_files_properties(
  m37710/m37710.cpp
  m37710/m37710o0.cpp
  m37710/m37710o1.cpp
  m37710/m37710o2.cpp
  m37710/m37710o3.cpp
  arcade/third_party/adsp2181/adsp2181_core.c
  musashi/m68kcpu.c
  musashi/m68kops.c
  PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)
# ArcadeDuck reviewed imported/adapted core warning isolation (MSVC only).
# Keep these warnings active for normal ArcadeDuck-maintained code.
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set_source_files_properties(
    arcade/systems/konami/gq/konami_gq_tms57002_ops.cpp
    arcade/systems/sony/zn/taito_fx1b_mn10200.cpp
    PROPERTIES COMPILE_OPTIONS "/wd4244"
  )

  set_source_files_properties(
    arcade/third_party/adsp2181/adsp2181_core.c
    PROPERTIES COMPILE_OPTIONS "/wd4018;/wd4100;/wd4244;/wd4245;/wd4389"
  )

  set_source_files_properties(
    arcade/third_party/qsound_hle/qsound.cpp
    PROPERTIES COMPILE_OPTIONS "/wd4244;/wd4456"
  )

  set_source_files_properties(
    arcade/third_party/ymfm/ymfm_adpcm.cpp
    arcade/third_party/ymfm/ymfm_opn.cpp
    arcade/third_party/ymfm/ymfm_ssg.cpp
    PROPERTIES COMPILE_OPTIONS "/wd4244;/wd4389"
  )

  set_source_files_properties(
    arcade/third_party/z80_superzazu/z80.cpp
    PROPERTIES COMPILE_OPTIONS "/wd4244;/wd4805"
  )

  set_source_files_properties(
    m37710/m37710.cpp
    m37710/m37710o0.cpp
    m37710/m37710o1.cpp
    m37710/m37710o2.cpp
    m37710/m37710o3.cpp
    PROPERTIES COMPILE_OPTIONS "/wd4018;/wd4100;/wd4244"
  )

  set_source_files_properties(
    musashi/m68kcpu.c
    musashi/m68kops.c
    PROPERTIES COMPILE_OPTIONS "/wd4101;/wd4189;/wd4244;/wd4245;/wd4702"
  )
endif()
target_include_directories(core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(core PUBLIC Threads::Threads common util)
target_link_libraries(core PRIVATE xxhash imgui minizip rapidyaml rcheevos cpuinfo::cpuinfo ZLIB::ZLIB Zstd::Zstd libchdr)

if(CPU_ARCH_X64)
  target_compile_definitions(core PUBLIC "ENABLE_RECOMPILER=1" "ENABLE_NEWREC=1" "ENABLE_MMAP_FASTMEM=1")
  target_sources(core PRIVATE ${RECOMPILER_SRCS} ${NEWREC_SOURCES}
    cpu_recompiler_code_generator_x64.cpp
    cpu_newrec_compiler_x64.cpp
    cpu_newrec_compiler_x64.h
  )
  target_link_libraries(core PRIVATE xbyak)
  if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    target_link_libraries(core PRIVATE zydis)
  endif()
  message(STATUS "Building x64 recompiler.")
endif()
if(CPU_ARCH_ARM32)
  target_compile_definitions(core PUBLIC "ENABLE_RECOMPILER=1" "ENABLE_NEWREC=1")
  target_sources(core PRIVATE ${RECOMPILER_SRCS} ${NEWREC_SOURCES}
    cpu_recompiler_code_generator_aarch32.cpp
    cpu_newrec_compiler_aarch32.cpp
    cpu_newrec_compiler_aarch32.h
  )
  target_link_libraries(core PUBLIC vixl)
  message(STATUS "Building AArch32 recompiler.")
endif()
if(CPU_ARCH_ARM64)
  target_compile_definitions(core PUBLIC "ENABLE_RECOMPILER=1" "ENABLE_NEWREC=1" "ENABLE_MMAP_FASTMEM=1")
  target_sources(core PRIVATE ${RECOMPILER_SRCS} ${NEWREC_SOURCES}
    cpu_recompiler_code_generator_aarch64.cpp
    cpu_newrec_compiler_aarch64.cpp
    cpu_newrec_compiler_aarch64.h
  )
  target_link_libraries(core PUBLIC vixl)
  message(STATUS "Building AArch64 recompiler.")
endif()
if(CPU_ARCH_RISCV64)
  target_compile_definitions(core PUBLIC "ENABLE_NEWREC=1" "ENABLE_MMAP_FASTMEM=1")
  target_sources(core PRIVATE ${NEWREC_SOURCES}
    cpu_newrec_compiler_riscv64.cpp
    cpu_newrec_compiler_riscv64.h
  )
  target_link_libraries(core PUBLIC biscuit::biscuit riscv-disas)
  message(STATUS "Building RISC-V 64-bit recompiler.")
endif()

# Copy the provided data directory to the output directory. Borrowed from PCSX2.
function(add_resources target path basedir)
	get_filename_component(dir ${path} DIRECTORY)
	file(RELATIVE_PATH subdir ${basedir} ${dir})
	if(APPLE)
		target_sources(${target} PRIVATE ${path})
		set_source_files_properties(${path} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/${subdir})
	else()
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/resources/${subdir}"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${path}" "$<TARGET_FILE_DIR:${target}>/resources/${subdir}")
	endif()
	source_group(Resources/${subdir} FILES ${path})
endfunction()

function(add_core_resources target)
  add_util_resources(${target})

  if(APPLE)
    # Copy discord-rpc into the bundle
    get_target_property(DISCORD_RPC_LIBRARY DiscordRPC::discord-rpc IMPORTED_LOCATION_RELEASE)
    target_sources(${target} PRIVATE "${DISCORD_RPC_LIBRARY}")
    set_source_files_properties("${DISCORD_RPC_LIBRARY}" PROPERTIES MACOSX_PACKAGE_LOCATION Frameworks)
  endif()

  file(GLOB_RECURSE RESOURCE_FILES ${CMAKE_SOURCE_DIR}/data/resources/*)
  foreach(path IN LISTS RESOURCE_FILES)
    get_filename_component(file ${path} NAME)
    if("${file}" MATCHES "^\\.") # Don't copy macOS garbage (mainly Finder's .DS_Store files) into application
      continue()
    endif()
    add_resources(${target} ${path} ${CMAKE_SOURCE_DIR}/data/resources/)
  endforeach()
  if(ALLOW_INSTALL)
    install_imported_dep_library(cpuinfo::cpuinfo)
    install(DIRECTORY "$<TARGET_FILE_DIR:${target}>/resources" DESTINATION "${CMAKE_INSTALL_PREFIX}")
  endif()
endfunction()
