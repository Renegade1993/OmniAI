# dll_to_def.cmake - generate a .def file from a DLL export table via dumpbin.
# Args: -DDLL=<path to dll> -DDUMPBIN=<dumpbin.exe> -DDEF=<output .def>
execute_process(
	COMMAND ${DUMPBIN} /NOLOGO /EXPORTS ${DLL}
	OUTPUT_VARIABLE _dump
	OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _dump)
	message(FATAL_ERROR "dumpbin produced no output for ${DLL}")
endif()

file(WRITE ${DEF} "LIBRARY VCMI_lib\nEXPORTS\n")
set(_inExports OFF)
string(REPLACE "\n" ";" _lines "${_dump}")
foreach(_line IN LISTS _lines)
	if(_line MATCHES "ordinal +hint")
		set(_inExports ON)
		continue()
	endif()
	if(_inExports)
		# lines look like: "      1234 0x00001234 ?foo@@... (decorated name last)"
		string(STRIP "${_line}" _line)
		if(_line STREQUAL "")
			continue()
		endif()
		if(_line MATCHES "^Summary")
			break()
		endif()
		string(REGEX MATCH "[ \t]+([^ \t]+)$" _m "${_line}")
		if(CMAKE_MATCH_1)
			file(APPEND ${DEF} "  ${CMAKE_MATCH_1}\n")
		endif()
	endif()
endforeach()
