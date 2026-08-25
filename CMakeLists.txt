cmake_minimum_required(VERSION 3.20)
project(SAFSYNCore VERSION 0.1.0 LANGUAGES CXX)

option(SAFSYN_BUILD_TESTS "Build the automated test suite" ON)
option(SAFSYN_BUILD_WINMM "Build the experimental Windows DLL shell" OFF)

add_library(safsyn STATIC
	SAFSYN/core.cpp
	SAFSYN/engine.cpp
	SAFSYN/wav_writer.cpp
)
add_library(SAFSYN::safsyn ALIAS safsyn)
target_include_directories(safsyn PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/SAFSYN")
target_compile_features(safsyn PUBLIC cxx_std_20)
set_target_properties(safsyn PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
	target_compile_options(safsyn PRIVATE /W4 /permissive- /EHsc)
	target_compile_definitions(safsyn PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
	target_compile_options(safsyn PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(safsyn-render tools/safsyn_render.cpp)
target_link_libraries(safsyn-render PRIVATE SAFSYN::safsyn)
target_compile_features(safsyn-render PRIVATE cxx_std_20)
if(MSVC)
	target_compile_options(safsyn-render PRIVATE /EHsc)
endif()

if(WIN32 AND SAFSYN_BUILD_WINMM)
	add_library(safsyn-winmm MODULE SAFSYN/dllmain.cpp)
	target_link_libraries(safsyn-winmm PRIVATE SAFSYN::safsyn)
	target_compile_features(safsyn-winmm PRIVATE cxx_std_20)
endif()

if(SAFSYN_BUILD_TESTS)
	include(CTest)
	add_executable(safsyn-tests tests/core_tests.cpp)
	target_link_libraries(safsyn-tests PRIVATE SAFSYN::safsyn)
	target_compile_features(safsyn-tests PRIVATE cxx_std_20)
	if(MSVC)
		target_compile_options(safsyn-tests PRIVATE /EHsc)
	endif()
	add_test(NAME safsyn-core COMMAND safsyn-tests "${CMAKE_CURRENT_BINARY_DIR}/test-data")
	add_test(NAME safsyn-demo-render
		COMMAND safsyn-render --demo "${CMAKE_CURRENT_BINARY_DIR}/coherent-demo.wav")
endif()
