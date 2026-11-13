# libmenhir.so: the app's native side. Built by Gradle (android/app/build.gradle.kts, externalNativeBuild).
cmake_minimum_required(VERSION 3.22)
project(menhir_native C)
set(CMAKE_C_STANDARD 11)

get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# The pretend engine (fake_vm.c) until the real one runs on Android.
option(MENHIR_FAKE_ENGINE "Use fake_vm.c in place of the engine" ON)
# The pretend downloader (fake_fetch.c). Off with the fake engine to test real downloads.
option(MENHIR_FAKE_FETCH "Use fake_fetch.c in place of host/menhir_fetch from menhir::http" ${MENHIR_FAKE_ENGINE})

add_library(menhir SHARED jni.c)
target_include_directories(menhir PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${ROOT}/host)
target_compile_options(menhir PRIVATE -Wall -Wextra)
target_link_libraries(menhir PRIVATE android log)
# 16 KB-aligned segments, which Android 15 asks of native code (and a 16 KB-page kernel needs).
target_link_options(menhir PRIVATE -Wl,-z,max-page-size=16384)

if(MENHIR_FAKE_ENGINE)
  target_sources(menhir PRIVATE fake_vm.c)
  target_compile_definitions(menhir PRIVATE MENHIR_FAKE_ENGINE)
else()
  target_sources(menhir PRIVATE henge-vm.c)
  # ---- The real engine: add the other areas' targets here ----------------------------------------
  # android/engine/ (engine), android/gfx/ (Vulkan display: hv_vm_set_display_window), android/deps/
  # (restore stack, slirp, audio). Each is expected to define the alias target named below.
  #   add_subdirectory(${ROOT}/android/engine ${CMAKE_BINARY_DIR}/engine)
  #   add_subdirectory(${ROOT}/android/gfx    ${CMAKE_BINARY_DIR}/gfx)
  #   add_subdirectory(${ROOT}/android/deps   ${CMAKE_BINARY_DIR}/deps)
  #   target_link_libraries(menhir PRIVATE menhir::engine menhir::gfx menhir::restore menhir::net menhir::audio)
  # -----------------------------------------------------------------------------------------------
  message(FATAL_ERROR "MENHIR_FAKE_ENGINE=OFF needs the engine targets: add them above")
endif()

if(MENHIR_FAKE_FETCH)
  target_sources(menhir PRIVATE fake_fetch.c)
else()
  # host/menhir_fetch.h, implemented by the services agent.
  #   target_link_libraries(menhir PRIVATE menhir::http)
  message(FATAL_ERROR "MENHIR_FAKE_FETCH=OFF needs menhir::http: add it above")
endif()
