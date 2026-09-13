# MusicLibrary.Application -- use cases, orchestration and the shared engine.

add_library(MusicLibraryApplication STATIC
	src/Catalogue.cpp
	src/Library.cpp
	src/LibraryEnrichment.cpp
)

add_library(MusicLibrary::Application ALIAS MusicLibraryApplication)

target_include_directories(MusicLibraryApplication
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
)

target_link_libraries(MusicLibraryApplication
	PUBLIC
		MusicLibrary::Core
		MusicLibrary::Infrastructure
	PRIVATE
		Threads::Threads
)

target_compile_features(MusicLibraryApplication PUBLIC cxx_std_20)
ml_apply_common_options(MusicLibraryApplication)
set_target_properties(MusicLibraryApplication PROPERTIES POSITION_INDEPENDENT_CODE ON)
