# MusicLibrary.Infrastructure -- SQLite, filesystem protection, tag adapters,
# image and audio pipelines, HTTP and provider adapters.
#
# Depends on Core and on native libraries, never on Qt Widgets: the CLI links
# this target and must run without a display server.

add_library(MusicLibraryInfrastructure STATIC
	src/Hashing.cpp
	src/PathGuard.cpp
	src/Sqlite.cpp
	src/Schema.cpp
	src/Mp3Container.cpp
	src/TagReader.cpp
	src/TagWriter.cpp
	src/ImagePipeline.cpp
	src/AudioAnalysis.cpp
	src/Http.cpp
	src/Json.cpp
	src/Providers.cpp
)

add_library(MusicLibrary::Infrastructure ALIAS MusicLibraryInfrastructure)

target_include_directories(MusicLibraryInfrastructure
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
)

target_link_libraries(MusicLibraryInfrastructure
	PUBLIC
		MusicLibrary::Core
		MusicLibrary::Version
	PRIVATE
		Resonance::TagLib
		SQLite::SQLite3
		JPEG::JPEG
		PNG::PNG
		CURL::libcurl
		ZLIB::ZLIB
		Threads::Threads
		minimp3
)

target_compile_features(MusicLibraryInfrastructure PUBLIC cxx_std_20)
ml_apply_common_options(MusicLibraryInfrastructure)
set_target_properties(MusicLibraryInfrastructure PROPERTIES POSITION_INDEPENDENT_CODE ON)
