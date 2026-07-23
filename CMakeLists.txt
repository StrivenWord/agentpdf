cmake_minimum_required(VERSION 3.20)
project(agentpdf VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Architecture detection for the optional x86 AVX2 pixel-conversion fast path.
# This path is intentionally isolated to a single source file and is never
# compiled on ARM64 or other non-x86 targets.
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86|x86|ia32)$")
  set(AGENTPDF_IS_X86 TRUE)
else()
  set(AGENTPDF_IS_X86 FALSE)
endif()
option(AGENTPDF_ENABLE_AVX2 "Enable AVX2 pixel-format fast path on x86-64/x86" ${AGENTPDF_IS_X86})

find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER_CPP REQUIRED IMPORTED_TARGET poppler-cpp)
pkg_check_modules(LEPT REQUIRED IMPORTED_TARGET lept)
pkg_check_modules(TESSERACT REQUIRED IMPORTED_TARGET tesseract)

set(AGENTPDF_SOURCES
  src/main.cpp
  src/cli/args.cpp
  src/interactive/repl.cpp
  src/config/load.cpp
  src/index/job_index.cpp
  src/pdf/document.cpp
  src/layout/continuity.cpp
  src/layout/leptonica_analyze.cpp
  src/ocr/tesseract_page.cpp
  src/text/paragraphs.cpp
  src/text/footnotes.cpp
  src/text/structure.cpp
  src/meta/extract.cpp
  src/markdown/assemble.cpp
  src/pipeline/convert.cpp
  src/report/run_stats.cpp
  src/util/json_mini.cpp
  src/util/fs_util.cpp
  src/util/string_util.cpp
  src/util/thread_pool.cpp
  src/util/simd.cpp
)

if (AGENTPDF_ENABLE_AVX2)
  list(APPEND AGENTPDF_SOURCES src/util/pixel_convert_x86.cpp)
  add_compile_definitions(AGENTPDF_HAS_AVX2_PATH)
endif()

add_executable(agentpdf ${AGENTPDF_SOURCES})

target_include_directories(agentpdf PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(agentpdf PRIVATE
  PkgConfig::POPPLER_CPP
  PkgConfig::LEPT
  PkgConfig::TESSERACT
)

if (AGENTPDF_ENABLE_AVX2)
  message(STATUS "agentpdf: AVX2 pixel-conversion fast path enabled")
  if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src/util/pixel_convert_x86.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
  elseif (MSVC)
    set_source_files_properties(src/util/pixel_convert_x86.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
  endif()
else()
  message(STATUS "agentpdf: AVX2 pixel-conversion fast path disabled")
endif()

# Ship default configs next to the build tree for local runs.
add_custom_command(TARGET agentpdf POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:agentpdf>/config
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/config
          $<TARGET_FILE_DIR:agentpdf>/config
)

enable_testing()
add_test(NAME word_order_crane
  COMMAND ${CMAKE_COMMAND}
    -DAGENTPDF=$<TARGET_FILE:agentpdf>
    -DPDF=${CMAKE_SOURCE_DIR}/../unit-testing/md-rendering/source-pdfs/acm_crane2001.pdf
    -DGOLD=${CMAKE_SOURCE_DIR}/../unit-testing/md-rendering/handmade-extractions/acm_crane2001_handmade.md
    -DOUTDIR=${CMAKE_BINARY_DIR}/test-out
    -DHEUR=${CMAKE_SOURCE_DIR}/config/overrides/acm_crane2001.heuristics.json
    -P ${CMAKE_SOURCE_DIR}/cmake/CheckWordOrder.cmake
)
add_test(NAME word_order_bernstein
  COMMAND ${CMAKE_COMMAND}
    -DAGENTPDF=$<TARGET_FILE:agentpdf>
    -DPDF=${CMAKE_SOURCE_DIR}/../unit-testing/md-rendering/source-pdfs/acm_bernstein2002.pdf
    -DGOLD=${CMAKE_SOURCE_DIR}/../unit-testing/md-rendering/handmade-extractions/acm_bernstein2002_handmade.md
    -DOUTDIR=${CMAKE_BINARY_DIR}/test-out
    -DHEUR=${CMAKE_SOURCE_DIR}/config/overrides/acm_bernstein2002.heuristics.json
    -P ${CMAKE_SOURCE_DIR}/cmake/CheckWordOrder.cmake
)
add_test(NAME word_order_candidate
  COMMAND ${CMAKE_COMMAND}
    -DAGENTPDF=$<TARGET_FILE:agentpdf>
    -DPDF=${CMAKE_SOURCE_DIR}/../unit-testing/md-rendering/source-pdfs/candidate_paper.pdf
    -DGOLD=${CMAKE_SOURCE_DIR}/../unit-testing/md-rendering/handmade-extractions/candidate_paper_handmade.md
    -DOUTDIR=${CMAKE_BINARY_DIR}/test-out
    -DHEUR=${CMAKE_SOURCE_DIR}/config/overrides/candidate_paper.heuristics.json
    -P ${CMAKE_SOURCE_DIR}/cmake/CheckWordOrder.cmake
)
add_test(NAME continuity_addis_smoke
  COMMAND ${CMAKE_COMMAND}
    -DAGENTPDF=$<TARGET_FILE:agentpdf>
    -DPDF=${CMAKE_SOURCE_DIR}/../unit-testing/md-rendering/source-pdfs/ccp_addis2000.pdf
    -DOUTDIR=${CMAKE_BINARY_DIR}/addis-smoke-out
    -P ${CMAKE_SOURCE_DIR}/cmake/CheckContinuitySmoke.cmake
)
