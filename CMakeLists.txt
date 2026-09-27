# OmniAI - omni-compatible VCMI adventure AI plugin.
#
# Dual-mode build:
#   In-tree: place/symlink this directory under vcmi/AI/ and add_subdirectory it.
#            We link VCMI's own `vcmi` target by name (lib/ defines it after AI/).
#   Standalone: configure with -DVCMI_ROOT=<vcmi source checkout> and
#            -DVCMI_BIN=<installed VCMI dir containing VCMI_lib.dll>.
#            We generate an import library from VCMI_lib.dll so the plugin links
#            exclusively against the facade without building vcmiMain.

cmake_minimum_required(VERSION 3.24...3.31)

if(NOT DEFINED VCMI_IN_TREE)
	# VCMI's top-level CMakeLists adds AI/ before lib/, so in-tree the vcmi
	# target does not exist yet when this file runs. VCMI's cmake_modules
	# define vcmi_set_output_dir before any subdirectory is added, and a
	# standalone build never defines it.
	if(TARGET vcmi OR COMMAND vcmi_set_output_dir)
		set(VCMI_IN_TREE ON)
	else()
		set(VCMI_IN_TREE OFF)
	endif()
endif()

if(NOT VCMI_IN_TREE)
	project(OmniAI CXX)

	set(CMAKE_CXX_STANDARD 20)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)

	if(NOT DEFINED VCMI_ROOT)
		set(VCMI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/vcmi")
	endif()
	if(NOT DEFINED VCMI_BIN)
		# Only the imported library's recorded location: nothing is linked or
		# copied from it. -DVCMI_BIN or the VCMI_BIN environment variable
		# names a VCMI install; without either it points at resources/,
		# beside the import library.
		if(DEFINED ENV{VCMI_BIN})
			set(VCMI_BIN "$ENV{VCMI_BIN}")
		else()
			set(VCMI_BIN "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
		endif()
	endif()

	# Import library generated once by gen_import_lib.bat into resources/.
	set(RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
	set(VCMI_IMPORT_LIB "${RESOURCES}/VCMI_lib.lib")
	if(NOT EXISTS "${VCMI_IMPORT_LIB}")
		message(FATAL_ERROR "VCMI_lib.lib missing. Run OmniAI/gen_import_lib.bat from a VS dev prompt once.")
	endif()

	add_library(vcmi SHARED IMPORTED GLOBAL)
	set_target_properties(vcmi PROPERTIES
		IMPORTED_LOCATION "${VCMI_BIN}/VCMI_lib.dll"
		IMPORTED_IMPLIB "${VCMI_IMPORT_LIB}"
		INTERFACE_INCLUDE_DIRECTORIES "${VCMI_ROOT}/lib;${VCMI_ROOT}/include;${VCMI_ROOT}"
	)
	# VCMI_LIB_NAMESPACE stays UNDEFINED on desktop builds; defining it to ""
	# makes 'namespace ""' parse as an anonymous namespace and breaks dllimport
	# on every class (C2201 vftable errors).

	# oneTBB from resources/ (downloaded release); fall back to find_package.
	find_package(TBB QUIET PATHS "${RESOURCES}/onetbb/oneapi-tbb-2022.2.0/lib/cmake/tbb")
	if(NOT TBB_FOUND)
		set(TBB_ROOT "${RESOURCES}/onetbb/oneapi-tbb-2022.2.0")
		add_library(TBB::tbb SHARED IMPORTED GLOBAL)
		set_target_properties(TBB::tbb PROPERTIES
			IMPORTED_LOCATION "${TBB_ROOT}/redist/intel64/vc14/tbb12.dll"
			IMPORTED_IMPLIB "${TBB_ROOT}/lib/intel64/vc14/tbb12.lib"
			INTERFACE_INCLUDE_DIRECTORIES "${TBB_ROOT}/include"
		)
	endif()

	# Boost headers from resources/boost_1_87_0 (downloaded source; headers only).
	find_path(BOOST_INC NAMES boost/version.hpp
		PATHS "${RESOURCES}/boost_1_87_0" "${RESOURCES}/boost" "$ENV{BOOST_ROOT}")
	if(BOOST_INC AND NOT TARGET Boost::headers)
		add_library(Boost::headers INTERFACE IMPORTED)
		set_target_properties(Boost::headers PROPERTIES
			INTERFACE_INCLUDE_DIRECTORIES "${BOOST_INC}"
			# We ship headers only (no built boost libs). The auto-link pragmas
			# would inject libboost_* references that cannot resolve.
			INTERFACE_COMPILE_DEFINITIONS "BOOST_ALL_NO_LIB")
	endif()
endif()

set(omniAI_SRCS
	main.cpp
	StdInc.cpp
	OmniAI.cpp
	eval/ShadowDAG.cpp
	eval/DamageBridge.cpp
	eval/CombatEvaluator.cpp
	eval/UtilityEvaluator.cpp
	memory/LearningStore.cpp
	net/ActionDispatcher.cpp
	net/PendingActionQueue.cpp
)

set(omniAI_HEADERS
	StdInc.h
	OmniAI.h
	memory/GenerationalHandle.h
	memory/QSBRReclaimer.h
	memory/LearningStore.h
	eval/DifficultyMatrix.h
	eval/ShadowDAG.h
	eval/DamageBridge.h
	eval/CombatEvaluator.h
	eval/UtilityEvaluator.h
	net/ActionDispatcher.h
	net/PendingActionQueue.h
)

if(ENABLE_STATIC_LIBS)
	add_library(OmniAI STATIC ${omniAI_SRCS} ${omniAI_HEADERS})
else()
	add_library(OmniAI SHARED ${omniAI_SRCS} ${omniAI_HEADERS})
	if(VCMI_IN_TREE)
		install(TARGETS OmniAI RUNTIME DESTINATION ${AI_LIB_DIR} LIBRARY DESTINATION ${AI_LIB_DIR})
	else()
		set_target_properties(OmniAI PROPERTIES
			RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AI")
	endif()
endif()

target_link_libraries(OmniAI PRIVATE vcmi)
if(TARGET TBB::tbb)
	target_link_libraries(OmniAI PRIVATE TBB::tbb)
endif()
if(TARGET Boost::headers)
	target_link_libraries(OmniAI PRIVATE Boost::headers)
elseif(Boost_FOUND)
	target_link_libraries(OmniAI PRIVATE Boost::boost)
endif()

target_include_directories(OmniAI PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(OmniAI PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(VCMI_IN_TREE)
	vcmi_set_output_dir(OmniAI "AI")
	enable_pch(OmniAI)
	# Built with the engine: default to the engine's own user folder (VCMIDirs) when
	# OMNIAI_DIR is unset. The standalone build keeps the Documents known-folder lookup.
	target_compile_definitions(OmniAI PRIVATE OMNIAI_IN_TREE)
endif()

# Standalone unit canaries: exercise the memory and net subsystems against
# the real vcmi headers + import lib without loading the engine.
option(OMNIAI_BUILD_TESTS "Build the OmniAI unit test executable" ON)
if(OMNIAI_BUILD_TESTS AND NOT VCMI_IN_TREE)
	add_executable(omniai_tests
		tests/test_main.cpp
		StdInc.cpp
		net/PendingActionQueue.cpp
		eval/ShadowDAG.cpp
		eval/UtilityEvaluator.cpp
		eval/DamageBridge.cpp
		memory/LearningStore.cpp
	)
	target_link_libraries(omniai_tests PRIVATE vcmi)
	if(TARGET TBB::tbb)
		target_link_libraries(omniai_tests PRIVATE TBB::tbb)
	endif()
	if(TARGET Boost::headers)
		target_link_libraries(omniai_tests PRIVATE Boost::headers)
	endif()
	target_include_directories(omniai_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()
