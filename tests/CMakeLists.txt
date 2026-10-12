option(BUILD_GMOCK "Builds the googlemock subproject" OFF)
option(INSTALL_GTEST "Enable installation of googletest. (Projects embedding googletest may want to turn this OFF.)" OFF)
if(WIN32)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE GTest::gtest_main)
        
target_include_directories(${PROJECT_NAME} PUBLIC inc)
target_sources(${PROJECT_NAME} PRIVATE
        src/CheatManagerTest.cpp
        src/ConfigFileTest.cpp
        src/div32_test.cpp
        src/test_stubs.cpp
        src/serialize_test.cpp
        src/AicaArmTest.cpp
        src/Sh4InterpreterTest.cpp
        src/MmuTest.cpp
        src/HttpTest.cpp
        src/IniFileTest.cpp
        src/hw/modem/v42Test.cpp
        src/hw/modem/v42bisTest.cpp
        src/hw/sh4/modules/TimerTest.cpp
        src/imgread/ChdTest.cpp
        src/imgread/CueTest.cpp
        src/imgread/GdiTest.cpp
        src/input/ButtonComboTest.cpp
        src/input/GamepadInputHandlingTest.cpp
        src/input/MultiBindMappingTest.cpp
        src/input/MouseTest.cpp
        src/input/InputDefTest.cpp
        src/input/InputMappingConfigFileTest.cpp
        src/input/InputSetTest.cpp
        src/input/SDLControllerMappingTest.cpp
        src/oslib/I18nTest.cpp
        src/rend/SoftwareRasterizerTest.cpp
        src/rend/HybridTileRendererTest.cpp
        src/util/PeriodicThreadTest.cpp
        src/util/TsQueueTest.cpp
        src/util/WorkerThreadTest.cpp)

# Keep the renderer correctness suite independently buildable.  This provides a
# fast oracle/differential target without linking the emulator or its frontends.
add_executable(flycast-renderer-tests EXCLUDE_FROM_ALL
        src/rend/SoftwareRasterizerTest.cpp
        src/rend/HybridTileRendererTest.cpp
        ../core/rend/soft/SoftwareRasterizer.cpp
        ../core/rend/soft/HybridTileRenderer.cpp)
target_include_directories(flycast-renderer-tests PRIVATE ../core)
target_link_libraries(flycast-renderer-tests PRIVATE
        GTest::gtest_main
        nowide::nowide)

if(APPLE AND NOT IOS)
    target_sources(flycast-renderer-tests PRIVATE
            ../core/rend/soft/HybridTileRenderer.mm)
    target_compile_definitions(flycast-renderer-tests PRIVATE TARGET_MAC)
    find_library(RENDERER_TEST_METAL_LIBRARY Metal REQUIRED)
    find_library(RENDERER_TEST_IOSURFACE_LIBRARY IOSurface REQUIRED)
    find_library(RENDERER_TEST_FOUNDATION_LIBRARY Foundation REQUIRED)
    target_link_libraries(flycast-renderer-tests PRIVATE
            ${RENDERER_TEST_METAL_LIBRARY}
            ${RENDERER_TEST_IOSURFACE_LIBRARY}
            ${RENDERER_TEST_FOUNDATION_LIBRARY})
endif()

add_test(NAME flycast-renderer-tests COMMAND flycast-renderer-tests)
