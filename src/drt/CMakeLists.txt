# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2021-2025, The OpenROAD Authors

include("openroad")

option(DEBUG_DRT_UNDERFLOW "Check for underflow in drt cost calculations" OFF)
option(ENABLE_DRT_REDESIGN
  "Build the redesigned DRT transactional state engine (Phase 1 scaffolding only). \
See topics/.../drt_redesign_plan.md."
  OFF)
option(ENABLE_DRT_REDESIGN_OVERLAY
  "Build the V2 transactional read/write overlay around drt. Requires \
ENABLE_DRT_REDESIGN. See topics/.../v2_drt_redesign_plan.md."
  OFF)

if(ENABLE_DRT_REDESIGN_OVERLAY AND NOT ENABLE_DRT_REDESIGN)
  message(FATAL_ERROR
    "ENABLE_DRT_REDESIGN_OVERLAY requires ENABLE_DRT_REDESIGN=ON.")
endif()

project(drt
  LANGUAGES CXX
)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR}/cmake)

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT error)

find_package(Boost CONFIG REQUIRED COMPONENTS serialization)
find_package(OpenMP REQUIRED)
find_package(VTune)

set(FLEXROUTE_HOME ${PROJECT_SOURCE_DIR})

swig_lib(NAME      drt
         NAMESPACE drt
         I_FILE    src/TritonRoute.i
         SCRIPTS   src/TritonRoute.tcl
)

add_library(drt_lib
  src/dr/FlexDR_conn.cpp
  src/dr/FlexDR_init.cpp
  src/dr/FlexDR.cpp
  src/dr/FlexDRFlow.cpp
  src/db/drObj/drNet.cpp
  src/dr/FlexDR_maze.cpp
  src/dr/FlexGridGraph_maze.cpp
  src/dr/FlexGridGraph.cpp
  src/dr/FlexDR_rq.cpp
  src/dr/FlexDR_end.cpp
  src/ta/FlexTA_end.cpp
  src/ta/FlexTA_init.cpp
  src/ta/FlexTA_rq.cpp
  src/ta/FlexTA_assign.cpp
  src/ta/FlexTA.cpp
  src/global.cpp
  src/gc/FlexGC_rq.cpp
  src/gc/FlexGC.cpp
  src/gc/FlexGC_init.cpp
  src/gc/FlexGC_main.cpp
  src/gc/FlexGC_eol.cpp
  src/gc/FlexGC_inf.cpp
  src/gc/FlexGC_cut.cpp
  src/gc/FlexGC_metspc.cpp
  src/db/drObj/drAccessPattern.cpp
  src/db/drObj/drPin.cpp
  src/db/drObj/drShape.cpp
  src/db/drObj/drVia.cpp
  src/db/infra/frTime.cpp
  src/db/infra/KDTree.cpp
  src/db/taObj/taShape.cpp
  src/db/obj/frShape.cpp
  src/db/obj/frInst.cpp
  src/db/obj/frVia.cpp
  src/db/obj/frAccess.cpp
  src/db/obj/frInstTerm.cpp
  src/db/obj/frNet.cpp
  src/db/tech/frConstraint.cc
  src/db/obj/frMarker.cpp
  src/db/tech/frLayer.cc
  src/frRegionQuery.cpp
  src/io/io_pin.cpp
  src/io/io.cpp
  src/io/GuideProcessor.cpp
  src/io/io_parser_helper.cpp
  src/pa/FlexPA_init.cpp
  src/pa/FlexPA.cpp
  src/pa/FlexPA_acc_point.cpp
  src/pa/FlexPA_acc_pattern.cpp
  src/pa/FlexPA_row_pattern.cpp
  src/pa/FlexPA_unique.cpp
  src/rp/FlexRP_init.cpp
  src/rp/FlexRP.cpp
  src/rp/FlexRP_prep.cpp
  src/distributed/frArchive.cpp
  src/distributed/drUpdate.cpp
  src/distributed/paUpdate.cpp
  src/TritonRoute.cpp
  src/frBaseTypes.cpp
  src/DesignCallBack.cpp
)

target_include_directories(drt_lib
  PUBLIC
    include

  PRIVATE
    src
)

target_link_libraries(drt_lib
  PUBLIC
    absl::synchronization
    stt_lib
    utl_lib
    dst_lib
    Threads::Threads
    OpenMP::OpenMP_CXX
    ${Boost_LIBRARIES}
    ZLIB::ZLIB
)

if(ENABLE_DRT_REDESIGN)
  target_sources(drt_lib
    PRIVATE
      src/redesign/Footprint.cpp
      src/redesign/PhysicalState.cpp
      src/redesign/simd/Geom.cpp
      src/redesign/legality/CpuDrcOracle.cpp
      src/redesign/legality/Predicates.cpp
      src/redesign/legality/RuleDeck.cpp
      src/redesign/legality/FlexConstraintTranslator.cpp
      src/redesign/legality/ClipDump.cpp
      src/redesign/legality/ClipDumpHook.cpp
      src/redesign/legality/GcWorkerClipBuilder.cpp
      src/redesign/legality/RuleDeckDump.cpp
      src/redesign/legality/CaptureRuleDeck.cpp
      src/redesign/legality/AuditReport.cpp
  )
  target_compile_definitions(drt_lib PRIVATE ENABLE_DRT_REDESIGN=1)

  if(ENABLE_DRT_REDESIGN_OVERLAY)
    target_sources(drt_lib
      PRIVATE
        src/redesign/overlay/MemoryBackedGeometryView.cpp
        src/redesign/overlay/MutableGeometryStore.cpp
        src/redesign/overlay/OracleCandidate.cpp
        src/redesign/overlay/OverlayGeometryView.cpp
        src/redesign/overlay/RegionQueryGeometryView.cpp
        src/redesign/overlay/ShadowDump.cpp
        src/redesign/overlay/SyntheticOracle.cpp
    )
    target_compile_definitions(drt_lib PRIVATE ENABLE_DRT_REDESIGN_OVERLAY=1)
  endif()

  add_executable(redesign_simd_test test/redesign_simd_test.cpp)
  target_link_libraries(redesign_simd_test PRIVATE drt_lib)
  target_include_directories(redesign_simd_test PRIVATE src)

  add_executable(redesign_legality_test test/redesign_legality_test.cpp)
  target_link_libraries(redesign_legality_test PRIVATE drt_lib)
  target_include_directories(redesign_legality_test PRIVATE src)

  add_executable(redesign_e2_audit test/redesign_e2_audit.cpp)
  target_link_libraries(redesign_e2_audit PRIVATE drt_lib)
  target_include_directories(redesign_e2_audit PRIVATE src)

  if(ENABLE_DRT_REDESIGN_OVERLAY)
    add_executable(redesign_overlay_test test/redesign_overlay_test.cpp)
    target_link_libraries(redesign_overlay_test PRIVATE drt_lib)
    target_include_directories(redesign_overlay_test PRIVATE src)
  endif()
endif()

target_sources(drt
  PRIVATE
    src/dr/FlexDR_graphics.cpp
    src/ta/FlexTA_graphics.cpp
    src/pa/FlexPA_graphics.cpp
    src/MakeTritonRoute.cpp
    src/GraphicsFactory.cpp
)

target_include_directories(drt
  PUBLIC
    include

  PRIVATE
    src
)

target_link_libraries(drt
  PUBLIC
    drt_lib
    gui
)

messages(
  TARGET drt
)

############################################################
# Compiler flags
############################################################
# Todo: add -Wextra and cleanup warnings
target_compile_options(drt
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -pedantic -Wcast-qual -Wredundant-decls -Wformat-security>
    $<$<CXX_COMPILER_ID:AppleClang>:-Wall -pedantic -Wcast-qual -Wredundant-decls -Wformat-security -Wno-gnu-zero-variadic-macro-arguments>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -pedantic -Wcast-qual -Wredundant-decls -Wformat-security -Wno-gnu-zero-variadic-macro-arguments>
  )

############################################################
# Unit testing
############################################################
if(ENABLE_TESTS)
  enable_testing()

  add_executable(trTest
    ${FLEXROUTE_HOME}/test/gcTest.cpp
    ${FLEXROUTE_HOME}/test/fixture.cpp
  )

  target_include_directories(trTest
    PRIVATE
    ${FLEXROUTE_HOME}/src
    ${OPENROAD_HOME}/include
  )

  target_link_libraries(trTest
    drt_lib
    odb
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
  )

  add_test(NAME trTest COMMAND trTest)
  add_dependencies(build_and_test trTest)

  if(DEBUG_DRT_UNDERFLOW)
    target_compile_definitions(drt
      PRIVATE
      DEBUG_DRT_UNDERFLOW=1
    )
  endif()
endif()

############################################################
# VTune ITT API
############################################################

if (VTune_FOUND)
  target_compile_definitions(drt
    PUBLIC
    HAS_VTUNE=1
  )

  target_link_libraries(drt
    PUBLIC
    VTune::VTune
  )

endif(VTune_FOUND)

if (Python3_FOUND AND BUILD_PYTHON)
  swig_lib(NAME          drt_py
           NAMESPACE     drt
           LANGUAGE      python
           I_FILE        src/TritonRoute-py.i
           SWIG_INCLUDES ${PROJECT_SOURCE_DIR}/include/drt
           SCRIPTS       ${CMAKE_CURRENT_BINARY_DIR}/drt_py.py
  )

  target_include_directories(drt_py
    PUBLIC
      include
  )

  target_link_libraries(drt_py
    PUBLIC
      drt
  )

endif()

add_subdirectory(test)
