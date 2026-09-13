cmake_minimum_required(VERSION 3.22)

# Single versioned source for every platform artifact (DIST-001, REL-001).
project(Resonance
	VERSION 0.1.0
	DESCRIPTION "Resonance: MP3 library catalogue, artwork, BPM, lyrics, privacy and gain cleaning, and safe organisation"
	HOMEPAGE_URL "https://github.com/blueangelcpt/mp3-tool"
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(ML_BUILD_DESKTOP "Build the Qt 6 Widgets desktop application" ON)
option(ML_BUILD_CLI "Build the command line application" ON)
option(ML_BUILD_TESTS "Build the test suite" ON)
option(ML_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ML_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ML_ENABLE_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
option(ML_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# ---------------------------------------------------------------------------
# Common compile options
# ---------------------------------------------------------------------------
function(ml_apply_common_options target)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
		if(ML_WARNINGS_AS_ERRORS)
			target_compile_options(${target} PRIVATE /WX)
		endif()
		target_compile_definitions(${target} PRIVATE
			NOMINMAX
			WIN32_LEAN_AND_MEAN
			UNICODE
			_UNICODE
			_CRT_SECURE_NO_WARNINGS
		)
	else()
		target_compile_options(${target} PRIVATE
			-Wall -Wextra -Wpedantic
			-Wcast-qual -Wconversion -Wsign-conversion
			-Wnon-virtual-dtor -Woverloaded-virtual
			-Wdouble-promotion -Wformat=2
		)
		if(ML_WARNINGS_AS_ERRORS)
			target_compile_options(${target} PRIVATE -Werror)
		endif()
	endif()

	if(ML_ENABLE_ASAN)
		if(MSVC)
			target_compile_options(${target} PRIVATE /fsanitize=address)
		else()
			target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
			target_link_options(${target} PRIVATE -fsanitize=address)
		endif()
	endif()

	if(ML_ENABLE_UBSAN AND NOT MSVC)
		target_compile_options(${target} PRIVATE
			-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
		target_link_options(${target} PRIVATE -fsanitize=undefined)
	endif()
endfunction()

# ---------------------------------------------------------------------------
# Version header, shared by the CLI, the desktop app and the packaging metadata
# ---------------------------------------------------------------------------
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.hpp.in"
	"${CMAKE_CURRENT_BINARY_DIR}/generated/mlversion/Version.hpp"
	@ONLY
)
add_library(MusicLibraryVersion INTERFACE)
target_include_directories(MusicLibraryVersion INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_library(MusicLibrary::Version ALIAS MusicLibraryVersion)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(SQLite3 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# TagLib discovery has to work three different ways.
#
# TagLib 2 ships a CMake config package, which is what vcpkg and Homebrew
# install and the only thing available on Windows, where pkg-config generally is
# not. Debian's libtag1-dev historically ships only a .pc file. Try the config
# package first and fall back to pkg-config, then expose one target name so
# nothing downstream has to know which route succeeded.
find_package(taglib CONFIG QUIET)

if(taglib_FOUND AND TARGET TagLib::tag)
	add_library(Resonance::TagLib ALIAS TagLib::tag)
	set(ML_TAGLIB_SOURCE "CMake config package")
	if(DEFINED taglib_VERSION)
		set(TAGLIB_VERSION "${taglib_VERSION}")
	endif()
else()
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET taglib)
	add_library(Resonance::TagLib ALIAS PkgConfig::TAGLIB)
	set(ML_TAGLIB_SOURCE "pkg-config")
endif()

if(ML_BUILD_DESKTOP)
	find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Multimedia)
	qt_standard_project_setup()
endif()

# Vendored, pinned single-header MP3 decoder (CC0). See native/DEPENDENCIES.md.
#
# SYSTEM marks it as a system include so this project's strict warning set does
# not fire on code it does not own. Our own translation units still compile at
# full strictness.
add_library(minimp3 INTERFACE)
target_include_directories(minimp3 SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minimp3")

# The sample type is part of minimp3's ABI: without this define mp3d_sample_t is
# int16_t, with it float. Setting it on the interface target means every
# translation unit agrees, rather than the one that happens to define it locally
# disagreeing with the one that does not.
target_compile_definitions(minimp3 INTERFACE MINIMP3_FLOAT_OUTPUT)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_subdirectory(src/MusicLibrary.Core)
add_subdirectory(src/MusicLibrary.Infrastructure)
add_subdirectory(src/MusicLibrary.Application)

if(ML_BUILD_CLI)
	add_subdirectory(src/MusicLibrary.Cli)
endif()

if(ML_BUILD_DESKTOP)
	add_subdirectory(src/MusicLibrary.Desktop)
endif()

if(ML_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Packaging (DIST-001)
# ---------------------------------------------------------------------------
include(Packaging)

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "Resonance ${PROJECT_VERSION}")
message(STATUS "  build type .......... ${CMAKE_BUILD_TYPE}")
message(STATUS "  desktop (Qt 6) ...... ${ML_BUILD_DESKTOP}")
message(STATUS "  cli ................. ${ML_BUILD_CLI}")
message(STATUS "  tests ............... ${ML_BUILD_TESTS}")
message(STATUS "  asan / ubsan ........ ${ML_ENABLE_ASAN} / ${ML_ENABLE_UBSAN}")
message(STATUS "  TagLib .............. ${TAGLIB_VERSION} (via ${ML_TAGLIB_SOURCE})")
message(STATUS "  SQLite .............. ${SQLite3_VERSION}")
if(ML_BUILD_DESKTOP)
	message(STATUS "  Qt .................. ${Qt6_VERSION}")
endif()
message(STATUS "")
