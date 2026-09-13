# DIST-001 / DIST-003 / REL-001: platform packaging.
#
# All four artifacts are produced from this one versioned source. Each generator
# is configured here; whether a given artifact has actually been built and
# installed on its target OS is recorded in docs/progress.md, never assumed.

include(GNUInstallDirs)

set(CPACK_PACKAGE_NAME "resonance")
set(CPACK_PACKAGE_VENDOR "blueangelcpt")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_CONTACT "blueangelcpt")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Resonance")
set(CPACK_STRIP_FILES ON)

# Uninstall must leave the user's music and their catalogue alone (DIST-003).
# Neither lives under the installation prefix: music is never touched by the
# installer, and the catalogue lives in the per-user data directory.
set(CPACK_PACKAGE_DESCRIPTION
"Resonance catalogues an MP3 collection, proposes artwork, BPM, lyrics and
metadata cleaning, and writes organised copies. The original collection is
read-only to every operation.

Uninstalling removes the application only. Your music, your catalogue and your
settings are left in place.")

if(WIN32)
	set(CPACK_GENERATOR "NSIS")
	set(CPACK_NSIS_PACKAGE_NAME "Resonance ${PROJECT_VERSION}")
	set(CPACK_NSIS_DISPLAY_NAME "Resonance")
	set(CPACK_NSIS_HELP_LINK "${PROJECT_HOMEPAGE_URL}")
	set(CPACK_NSIS_URL_INFO_ABOUT "${PROJECT_HOMEPAGE_URL}")
	set(CPACK_NSIS_MODIFY_PATH ON)
	set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
	# The application runs as an ordinary user even though installing needs
	# elevation, and installation never starts a rewrite.
	set(CPACK_NSIS_EXECUTABLES_DIRECTORY "${CMAKE_INSTALL_BINDIR}")
	if(ML_BUILD_DESKTOP)
		set(CPACK_NSIS_CREATE_ICONS_EXTRA
			"CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Resonance.lnk' '$INSTDIR\\\\${CMAKE_INSTALL_BINDIR}\\\\resonance-desktop.exe'")
		set(CPACK_NSIS_DELETE_ICONS_EXTRA
			"Delete '$SMPROGRAMS\\\\$START_MENU\\\\Resonance.lnk'")
	endif()

elseif(APPLE)
	set(CPACK_GENERATOR "DragNDrop")
	set(CPACK_DMG_VOLUME_NAME "Resonance ${PROJECT_VERSION}")
	set(CPACK_DMG_FORMAT "UDZO")
	# Separate Intel and Apple Silicon images are the initial target; a universal
	# build would need universal copies of every bundled dependency.
	set(CPACK_PACKAGE_FILE_NAME
		"resonance-${PROJECT_VERSION}-macos-${CMAKE_SYSTEM_PROCESSOR}")

else()
	set(CPACK_GENERATOR "DEB")
	set(CPACK_DEBIAN_PACKAGE_MAINTAINER "blueangelcpt")
	set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
	set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
	set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${PROJECT_HOMEPAGE_URL}")
	set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
	# Resolve the actual shared-library dependencies rather than guessing them.
	set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
	set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS ON)
	set(CPACK_PACKAGE_FILE_NAME
		"resonance_${PROJECT_VERSION}_amd64")

	# A .deb targets Debian-family distributions. That is not a claim to support
	# every Linux distribution; see docs/progress.md for what was actually tested.
	if(ML_BUILD_DESKTOP)
		install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/resonance.desktop"
			DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications" COMPONENT desktop)
		install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/resonance.svg"
			DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps"
			COMPONENT desktop)
	endif()
endif()

include(CPack)
