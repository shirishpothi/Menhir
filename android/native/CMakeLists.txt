# libmenhir.so: the app's native side. Built by Gradle (android/app/build.gradle.kts, externalNativeBuild).
cmake_minimum_required(VERSION 3.22)
project(menhir_native C CXX)
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
  # The real engine and services. EXCLUDE_FROM_ALL:
  # only what libmenhir.so links is built, not the engine's tests and command-line tool.
  set(MENHIR_WITH_NET ON CACHE BOOL "" FORCE)
  set(MENHIR_WITH_RESTORE ON CACHE BOOL "" FORCE)
  set(MENHIR_WITH_AUDIO ON CACHE BOOL "" FORCE)
  add_subdirectory(${ROOT}/android/engine ${CMAKE_BINARY_DIR}/engine EXCLUDE_FROM_ALL)
  # henge-vm.c is the bridge; vm_android.c is host/vm.c for Android (see its first comment).
  target_sources(menhir PRIVATE henge-vm.c vm_android.c)
  target_compile_options(menhir PRIVATE -ffixed-x18)
  target_link_libraries(menhir PRIVATE menhir-engine menhir::net menhir::restore menhir::audio menhir::compat)
  # Replace the headless backend only in this real-app build.
  get_target_property(engine_sources menhir-engine SOURCES)
  list(FILTER engine_sources EXCLUDE REGEX "/pvg_null[.]c$")
  set_property(TARGET menhir-engine PROPERTY SOURCES "${engine_sources}")
  add_subdirectory(${ROOT}/android/gfx ${CMAKE_BINARY_DIR}/gfx EXCLUDE_FROM_ALL)
  target_link_libraries(menhir-engine PUBLIC menhir_gfx)
  set_target_properties(menhir PROPERTIES LINKER_LANGUAGE CXX)
endif()

if(MENHIR_FAKE_FETCH)
  target_sources(menhir PRIVATE fake_fetch.c)
else()
  if(MENHIR_FAKE_ENGINE)
    set(MENHIR_SERVICES_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${ROOT}/android/deps ${CMAKE_BINARY_DIR}/services EXCLUDE_FROM_ALL)
  endif()
  target_link_libraries(menhir PRIVATE menhir::http) # host/menhir_fetch.h, from the services
endif()
