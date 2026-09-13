# Unit and integration tests.
#
# Each executable is one suite, registered with CTest so a failure fails the
# build pipeline rather than scrolling past.

function(ml_add_test name)
	add_executable(${name} ${ARGN})
	target_link_libraries(${name} PRIVATE MusicLibrary::Application MusicLibrary::Version)
	target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	ml_apply_common_options(${name})
	add_test(NAME ${name} COMMAND ${name})
	# A hung test is a failed test, not an indefinitely stalled pipeline.
	set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()

ml_add_test(CorePolicyTests CorePolicyTests.cpp)
ml_add_test(InfrastructureTests InfrastructureTests.cpp)
ml_add_test(PreservationTests PreservationTests.cpp)
