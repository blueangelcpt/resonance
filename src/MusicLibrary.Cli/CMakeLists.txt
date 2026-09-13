# MusicLibrary.Cli -- the command line application.
#
# Links no Qt Widgets and requires no display server (FRD section 3).

add_executable(resonance-cli src/main.cpp)

target_link_libraries(resonance-cli
	PRIVATE
		MusicLibrary::Application
		MusicLibrary::Version
)

set_target_properties(resonance-cli PROPERTIES OUTPUT_NAME "resonance")
ml_apply_common_options(resonance-cli)

install(TARGETS resonance-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT cli)
