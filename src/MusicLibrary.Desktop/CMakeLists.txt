# MusicLibrary.Desktop -- Qt 6 Widgets views and desktop composition.
#
# This is the only target that links Qt Widgets. It contains no policy: every
# action calls the same MusicLibrary::Application methods the CLI calls.

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_executable(resonance-desktop WIN32 MACOSX_BUNDLE
	src/main.cpp
	src/MainWindow.cpp
	src/MainWindow.hpp
	src/TaskRunner.cpp
	src/TaskRunner.hpp
	src/TrackTableModel.cpp
	src/TrackTableModel.hpp
	src/AlbumReviewWidget.cpp
	src/AlbumReviewWidget.hpp
	src/Theme.cpp
	src/Theme.hpp
	src/VfdSpectrumWidget.cpp
	src/VfdSpectrumWidget.hpp
	src/Workbench.cpp
	src/Workbench.hpp
	src/AudioPlayer.cpp
	src/AudioPlayer.hpp
	resources.qrc
)

if(WIN32)
	# The .exe's own icon (Explorer, taskbar, Alt-Tab) has to come from a
	# compiled-in resource; it can't be set at runtime the way the window icon
	# below can.
	target_sources(resonance-desktop PRIVATE "${CMAKE_SOURCE_DIR}/packaging/windows/resonance.rc")
elseif(APPLE)
	# The .app bundle's Finder/Dock icon likewise has to be a bundled resource,
	# referenced by MACOSX_BUNDLE_ICON_FILE below.
	set(ML_MACOS_ICON "${CMAKE_SOURCE_DIR}/packaging/macos/resonance.icns")
	target_sources(resonance-desktop PRIVATE "${ML_MACOS_ICON}")
	set_source_files_properties("${ML_MACOS_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
endif()

target_link_libraries(resonance-desktop
	PRIVATE
		MusicLibrary::Application
		MusicLibrary::Version
		Qt6::Core
		Qt6::Gui
		Qt6::Widgets
		Qt6::Multimedia
		Qt6::Svg
		minimp3
)

ml_apply_common_options(resonance-desktop)

set_target_properties(resonance-desktop PROPERTIES
	MACOSX_BUNDLE_BUNDLE_NAME "Resonance"
	MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
	MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
	MACOSX_BUNDLE_GUI_IDENTIFIER "net.blueangelcpt.resonance"
	MACOSX_BUNDLE_COPYRIGHT "GPL-3.0-or-later"
	MACOSX_BUNDLE_ICON_FILE "resonance.icns"
)

install(TARGETS resonance-desktop
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT desktop
	BUNDLE DESTINATION . COMPONENT desktop
)
