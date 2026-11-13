cmake_minimum_required(VERSION 3.24)
project(menhir_gfx C CXX)
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Menhir graphics targets Android arm64-v8a")
endif()
set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(DEPS "${CMAKE_CURRENT_LIST_DIR}/build")
set(MESA "${DEPS}/mesa-src")
set(MESA_OUT "${DEPS}/mesa-android")
set(LLVM "${DEPS}/llvm-android")
file(GLOB AIR_SOURCES CONFIGURE_DEPENDS "${ROOT}/devices/air/stela/*.c")
add_library(menhir_gfx STATIC
  ${ROOT}/devices/pvg_vulkan.c ${ROOT}/devices/pvg_vulkan_desc.c
  ${ROOT}/devices/air/air_library.c ${ROOT}/devices/air/air_spirv.c
  ${ROOT}/devices/air/ntv/nir_to_spirv.c ${ROOT}/devices/air/ntv/spirv_builder.c ${AIR_SOURCES})
target_compile_features(menhir_gfx PRIVATE c_std_11)
target_compile_definitions(menhir_gfx PRIVATE
  __STDC_CONSTANT_MACROS
  __STDC_FORMAT_MACROS
  __STDC_LIMIT_MACROS
  HAVE_PTHREAD
  HAVE___BUILTIN_CLZ
  HAVE___BUILTIN_CLZLL
  HAVE___BUILTIN_CTZ
  HAVE___BUILTIN_EXPECT
  HAVE___BUILTIN_FFS
  HAVE___BUILTIN_FFSLL
  HAVE___BUILTIN_POPCOUNT
  HAVE___BUILTIN_POPCOUNTLL
  HAVE___BUILTIN_UNREACHABLE
  HAVE___BUILTIN_TYPES_COMPATIBLE_P
  HAVE_FUNC_ATTRIBUTE_PACKED
  HAVE_FUNC_ATTRIBUTE_CONST
  HAVE_FUNC_ATTRIBUTE_PURE
  HAVE_FUNC_ATTRIBUTE_UNUSED
  HAVE_FUNC_ATTRIBUTE_NORETURN
  HAVE_FUNC_ATTRIBUTE_FORMAT
  HAVE_FUNC_ATTRIBUTE_WARN_UNUSED_RESULT
  HAVE_FUNC_ATTRIBUTE_MALLOC
  HAVE_FUNC_ATTRIBUTE_RETURNS_NONNULL
  HAVE_FUNC_ATTRIBUTE_VISIBILITY
  USE_GCC_ATOMIC_BUILTINS
  NDEBUG
  MESA_DEBUG=0
  HAVE_STRUCT_TIMESPEC)
target_compile_options(menhir_gfx PRIVATE -fno-math-errno -ffunction-sections -fdata-sections
  -Wno-unused-function -Wno-unused-variable -Wno-missing-field-initializers)
target_include_directories(menhir_gfx PRIVATE
  ${ROOT}/core ${ROOT}/host ${ROOT}/devices ${ROOT}/devices/air
  ${ROOT}/devices/air/stela ${ROOT}/devices/air/stela/llvm/include ${ROOT}/devices/air/ntv
  ${LLVM}/include ${DEPS}/llvm-src/llvm/include
  ${MESA}/include ${MESA}/src ${MESA}/src/compiler ${MESA}/src/compiler/nir
  ${MESA}/src/gallium/include ${MESA}/src/gallium/auxiliary
  ${MESA_OUT}/src ${MESA_OUT}/src/compiler ${MESA_OUT}/src/compiler/nir ${MESA_OUT}/src/util)
set(MESA_LIBS ${MESA_OUT}/src/compiler/nir/libnir.a ${MESA_OUT}/src/compiler/libcompiler.a
  ${MESA_OUT}/src/util/libmesa_util.a ${MESA_OUT}/src/util/blake3/libblake3.a
  ${MESA_OUT}/src/c11/impl/libmesa_util_c11.a)
file(GLOB LLVM_LIBS "${LLVM}/lib/libLLVM*.a")
if(NOT LLVM_LIBS)
  message(FATAL_ERROR "Run android/gfx/llvm/build.sh and android/gfx/mesa/build.sh android first")
endif()
target_link_libraries(menhir_gfx PUBLIC "-Wl,--start-group" ${MESA_LIBS} ${LLVM_LIBS}
  "-Wl,--end-group" vulkan android log z m)
add_executable(pvg_replay_vulkan replay/pvg_replay.c ${ROOT}/devices/pvg_lite.c ${ROOT}/devices/pvg_vulkan_capture.c)
target_include_directories(pvg_replay_vulkan PRIVATE ${ROOT}/core ${ROOT}/host ${ROOT}/devices)
target_link_libraries(pvg_replay_vulkan PRIVATE menhir_gfx)
set_target_properties(pvg_replay_vulkan PROPERTIES LINKER_LANGUAGE CXX)
target_link_options(pvg_replay_vulkan PRIVATE -Wl,--gc-sections -static-libstdc++)
