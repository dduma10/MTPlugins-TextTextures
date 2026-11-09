cmake_minimum_required(VERSION 3.16)
project(TextTextures CXX)

# x64 Windows DLLs for the MTPlugins RadiantPlugins host (BO3 Mod Tools): Text Textures.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMMON_SOURCES common/Platform.cpp common/Png.cpp common/GdtWriter.cpp common/XModel.cpp common/Radiant.cpp common/MapEdit.cpp)

function(add_plugin name)
    add_library(${name} SHARED ${ARGN} ${COMMON_SOURCES})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/${name} ${CMAKE_CURRENT_SOURCE_DIR}/common ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
    target_compile_definitions(${name} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
    target_link_libraries(${name} PRIVATE user32 gdi32 comdlg32 shell32 ole32)
    set_target_properties(${name} PROPERTIES PREFIX "" OUTPUT_NAME ${name})
    if(MSVC)
        target_compile_options(${name} PRIVATE /utf-8 /W3)
        set_property(TARGET ${name} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_link_options(${name} PRIVATE -static)
    endif()
endfunction()

if(WIN32)
    add_plugin(TextTextures TextTextures/TextTextures.cpp TextTextures/TextRender.cpp TextTextures/Text3D.cpp)
endif()

# Platform-independent tests: build anywhere with -DMTPLUGINS_TESTS=ON, run with ctest.
option(MTPLUGINS_TESTS "Build the logic tests" OFF)
if(MTPLUGINS_TESTS)
    enable_testing()
    set(TEST_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/common ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
    add_executable(TextLogicTests TextTextures/Tests/LogicTests.cpp common/MapEdit.cpp common/GdtWriter.cpp common/Png.cpp)
    target_include_directories(TextLogicTests PRIVATE ${TEST_INCLUDES} TextTextures)
    add_executable(Text3DTests TextTextures/Tests/Text3DTests.cpp TextTextures/Text3D.cpp TextTextures/TextRender.cpp common/XModel.cpp)
    target_include_directories(Text3DTests PRIVATE ${TEST_INCLUDES} TextTextures)
    add_test(NAME TextLogicTests COMMAND TextLogicTests)
    add_test(NAME Text3DTests COMMAND Text3DTests)
endif()
