# MusicLibrary.Core -- domain models, requirement policies, pure matching and
# planning logic. This target must never depend on Qt, SQLite, libcurl or any
# platform-specific API (FRD section 15).

add_library(MusicLibraryCore STATIC
	src/TagModel.cpp
	src/Text.cpp
	src/NamingTemplate.cpp
	src/TagPolicy.cpp
	src/ArtworkPolicy.cpp
	src/AlbumGrouping.cpp
	src/Enrichment.cpp
	src/ChangePlan.cpp
)

add_library(MusicLibrary::Core ALIAS MusicLibraryCore)

target_include_directories(MusicLibraryCore
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
)

target_compile_features(MusicLibraryCore PUBLIC cxx_std_20)
ml_apply_common_options(MusicLibraryCore)

set_target_properties(MusicLibraryCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
