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
        src/rend/PresentationFrameTest.cpp
        src/rend/HybridTileRendererTest.cpp
        src/rend/TextureDecodeTest.cpp
        src/util/PeriodicThreadTest.cpp
        src/util/TsQueueTest.cpp
        src/util/WorkerThreadTest.cpp)

# Keep the renderer correctness suite independently buildable.  This provides a
# fast oracle/differential target without linking the emulator or its frontends.
add_executable(flycast-renderer-tests EXCLUDE_FROM_ALL
        src/rend/SoftwareRasterizerTest.cpp
        src/rend/PresentationFrameTest.cpp
        src/rend/HybridTileRendererTest.cpp
        src/rend/TextureDecodeTest.cpp
        ../core/rend/texconv.cpp
        ../core/rend/soft/SoftwareRasterizer.cpp
        ../core/rend/soft/HybridTileRenderer.cpp)
target_include_directories(flycast-renderer-tests PRIVATE
        ../core
        ../core/deps
        ../core/deps/glm)
target_link_libraries(flycast-renderer-tests PRIVATE
        GTest::gtest_main
        nowide::nowide
        xxHash::xxhash)
target_compile_definitions(flycast-renderer-tests PRIVATE FLYCAST_TEXCONV_STANDALONE)

if(APPLE AND NOT IOS)
  if(NOT LIBRETRO)
   if(TARGET SDL2::SDL2)
    add_executable(flycast-metal-window-probe EXCLUDE_FROM_ALL
            src/rend/MetalWindowProbe.mm ../core/rend/soft/MetalPresentationFrame.mm)
    target_include_directories(flycast-metal-window-probe PRIVATE ../core ../core/deps ../core/deps/glm)
    target_compile_options(flycast-metal-window-probe PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(flycast-metal-window-probe PRIVATE SDL2::SDL2 nowide::nowide
            "-framework Metal" "-framework Foundation" "-framework QuartzCore")
   endif()
    add_library(flycast-metal-ui-support STATIC EXCLUDE_FROM_ALL
            ../core/rend/soft/MetalPresentationFrame.mm
            ../core/rend/soft/MetalUiDriver.mm
            ../core/deps/imgui/backends/imgui_impl_metal.mm
            ../core/deps/imgui/imgui.cpp
            ../core/deps/imgui/imgui_draw.cpp
            ../core/deps/imgui/imgui_cjk.cpp
            ../core/deps/imgui/misc/freetype/imgui_freetype.cpp
            ../core/deps/imgui/imgui_tables.cpp
            ../core/deps/imgui/imgui_widgets.cpp)
    target_include_directories(flycast-metal-ui-support PUBLIC ../core/deps/imgui ../core ../core/deps ../core/deps/glm)
    target_compile_options(flycast-metal-ui-support PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(flycast-metal-ui-support PUBLIC freetype nowide::nowide
            "-framework Metal" "-framework Foundation" "-framework QuartzCore")
    add_executable(flycast-metal-ui-tests EXCLUDE_FROM_ALL src/rend/MetalUiTest.mm)
    target_compile_options(flycast-metal-ui-tests PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(flycast-metal-ui-tests PRIVATE flycast-metal-ui-support GTest::gtest_main)
    if(TARGET SDL2::SDL2)
        add_executable(flycast-metal-driver-probe EXCLUDE_FROM_ALL src/rend/MetalDriverProbe.mm)
        target_compile_options(flycast-metal-driver-probe PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
        target_link_libraries(flycast-metal-driver-probe PRIVATE flycast-metal-ui-support SDL2::SDL2)
    endif()
  endif()
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
