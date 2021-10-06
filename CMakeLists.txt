cmake_minimum_required (VERSION 3.0)
project (sdtui VERSION 0.1.0 LANGUAGES C)

# Moar warnings
if ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUCC)
	set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99")
	set (CMAKE_C_FLAGS_DEBUG
		"${CMAKE_C_FLAGS_DEBUG} -Wall -Wextra -Wno-missing-field-initializers")
endif ()

# For custom modules
set (CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

# Dependencies
find_package (ZLIB REQUIRED)
find_package (Ncursesw REQUIRED)
find_package (PkgConfig REQUIRED)
pkg_check_modules (dependencies REQUIRED glib-2.0>=2.38 gio-2.0 pango)

pkg_check_modules (icu icu-uc icu-i18n)
if (NOT icu_FOUND)
	find_program (icu_CONFIG_EXECUTABLE icu-config)
	if (NOT icu_CONFIG_EXECUTABLE)
		message (FATAL_ERROR "ICU not found")
	endif ()

	execute_process (COMMAND ${icu_CONFIG_EXECUTABLE} --cppflags
		OUTPUT_VARIABLE icu_CPPFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
	separate_arguments (icu_CPPFLAGS)

	# target_link_libraries() handles linker flags as well
	execute_process (COMMAND ${icu_CONFIG_EXECUTABLE} --ldflags
		OUTPUT_VARIABLE icu_LIBRARIES OUTPUT_STRIP_TRAILING_WHITESPACE)
	separate_arguments (icu_LIBRARIES)

	# Filter out include directories from the preprocessor flags
	set (icu_INCLUDE_DIRS)
	foreach (flag ${icu_CPPFLAGS})
		if (flag MATCHES "^-I(.*)")
			list (APPEND icu_INCLUDE_DIRS "${CMAKE_MATCH_1}")
		endif ()
	endforeach ()

	# This should suffice most of the time, don't care about the rest
endif ()

find_package (Termo QUIET NO_MODULE)
option (USE_SYSTEM_TERMO
	"Don't compile our own termo library, use the system one" ${Termo_FOUND})

if (USE_SYSTEM_TERMO)
	if (NOT Termo_FOUND)
		message (FATAL_ERROR "System termo library not found")
	endif ()
else ()
	# We don't want the library to install, even though EXCLUDE_FROM_ALL
	# sabotages CTest -- those unbuilt tests need to be excluded in CTest runs
	add_subdirectory (termo EXCLUDE_FROM_ALL)
	# We don't have many good choices; this is a relatively clean approach
	# (other possibilities: setting a variable in the parent scope, using
	# a cache variable, writing a special config file with build paths in it
	# and including it here, or setting a custom property on the targets)
	get_directory_property (Termo_INCLUDE_DIRS
		DIRECTORY termo INCLUDE_DIRECTORIES)
	set (Termo_LIBRARIES termo-static)
endif ()

pkg_check_modules (xcb xcb xcb-xfixes)
option (WITH_X11 "Compile with X11 selection support using XCB" ${xcb_FOUND})

if (WITH_X11)
	if (NOT xcb_FOUND)
		message (FATAL_ERROR "XCB not found")
	endif ()

	list (APPEND dependencies_INCLUDE_DIRS ${xcb_INCLUDE_DIRS})
	list (APPEND dependencies_LIBRARY_DIRS ${xcb_LIBRARY_DIRS})
	list (APPEND dependencies_LIBRARIES ${xcb_LIBRARIES})
endif ()

link_directories (${dependencies_LIBRARY_DIRS})
include_directories (${ZLIB_INCLUDE_DIRS} ${icu_INCLUDE_DIRS}
	${dependencies_INCLUDE_DIRS} ${Ncursesw_INCLUDE_DIRS}
	${Termo_INCLUDE_DIRS})

# Configuration
include (CheckFunctionExists)
set (CMAKE_REQUIRED_LIBRARIES ${Ncursesw_LIBRARIES})
CHECK_FUNCTION_EXISTS ("resizeterm" HAVE_RESIZETERM)

# Localization
find_package (Gettext REQUIRED)
file (GLOB project_PO_FILES ${PROJECT_SOURCE_DIR}/po/*.po)
GETTEXT_CREATE_TRANSLATIONS (
	${PROJECT_SOURCE_DIR}/po/${PROJECT_NAME}.pot
	ALL ${project_PO_FILES})

# Documentation
find_program (ASCIIDOCTOR_EXECUTABLE asciidoctor)
if (NOT ASCIIDOCTOR_EXECUTABLE)
	message (FATAL_ERROR "asciidoctor not found")
endif ()

foreach (page "${PROJECT_NAME}.1")
	set (page_output "${PROJECT_BINARY_DIR}/${page}")
	list (APPEND project_MAN_PAGES "${page_output}")
	add_custom_command (OUTPUT ${page_output}
		COMMAND ${ASCIIDOCTOR_EXECUTABLE} -b manpage
			-a release-version=${PROJECT_VERSION}
			"${PROJECT_SOURCE_DIR}/docs/${page}.adoc"
			-o "${page_output}"
		DEPENDS "docs/${page}.adoc"
		COMMENT "Generating man page for ${page}" VERBATIM)
endforeach ()

add_custom_target (docs ALL DEPENDS ${project_MAN_PAGES})

# Project libraries
set (project_common_libraries ${ZLIB_LIBRARIES} ${icu_LIBRARIES}
	${dependencies_LIBRARIES} ${Ncursesw_LIBRARIES} termo-static)

set (project_common_headers
	${PROJECT_BINARY_DIR}/config.h
	src/dictzip-input-stream.h
	src/stardict.h
	src/stardict-private.h
	src/generator.h
	src/utils.h)

# Create a common project library so that source files are only compiled once
add_library (stardict OBJECT
	${project_common_headers}
	src/dictzip-input-stream.c
	src/generator.c
	src/stardict.c
	src/utils.c)
set (project_common_sources $<TARGET_OBJECTS:stardict>)

# Generate a configuration file
configure_file (${PROJECT_SOURCE_DIR}/config.h.in
	${PROJECT_BINARY_DIR}/config.h)
include_directories (${PROJECT_SOURCE_DIR} ${PROJECT_BINARY_DIR})

# Primary target source files
set (project_sources
	src/${PROJECT_NAME}.c)
set (project_headers
	${project_common_headers})

# Build the main executable and link it
add_definitions (-DGLIB_DISABLE_DEPRECATION_WARNINGS)
add_executable (${PROJECT_NAME}
	${project_sources} ${project_headers} ${project_common_sources})
target_link_libraries (${PROJECT_NAME} ${project_common_libraries})

# Experimental GTK+ frontend, we link it with ncurses but we don't care
pkg_check_modules (gtk gtk+-3.0)
if (gtk_FOUND)
	add_executable (sdgtk EXCLUDE_FROM_ALL
		src/sdgtk.c ${project_common_sources})
	target_include_directories (sdgtk PUBLIC ${gtk_INCLUDE_DIRS})
	target_link_libraries (sdgtk ${gtk_LIBRARIES} ${project_common_libraries})
endif ()

# Tools
set (tools tabfile add-pronunciation query-tool transform)
foreach (tool ${tools})
	add_executable (${tool} EXCLUDE_FROM_ALL
		src/${tool}.c ${project_common_sources})
	target_link_libraries (${tool} ${project_common_libraries})
endforeach ()

add_custom_target (tools DEPENDS ${tools})

# The files to be installed
include (GNUInstallDirs)
install (TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
install (FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})

foreach (page ${project_MAN_PAGES})
	string (REGEX MATCH "\\.([0-9])$" manpage_suffix "${page}")
	install (FILES "${page}"
		DESTINATION "${CMAKE_INSTALL_MANDIR}/man${CMAKE_MATCH_1}")
endforeach ()

# Do some unit tests
option (BUILD_TESTING "Build tests" OFF)
set (project_tests stardict)

if (BUILD_TESTING)
	enable_testing ()

	foreach (name ${project_tests})
		add_executable (test-${name}
			src/test-${name}.c ${project_common_sources})
		target_link_libraries (test-${name} ${project_common_libraries})
		add_test (test-${name} test-${name})
	endforeach ()
endif ()

# CPack
set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "StarDict terminal UI")
set (CPACK_PACKAGE_VENDOR "Premysl Eric Janouch")
set (CPACK_PACKAGE_CONTACT "Přemysl Eric Janouch <p@janouch.name>")
set (CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set (CPACK_GENERATOR "TGZ;ZIP")
set (CPACK_PACKAGE_FILE_NAME
	"${PROJECT_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set (CPACK_PACKAGE_INSTALL_DIRECTORY "${PROJECT_NAME}-${PROJECT_VERSION}")
set (CPACK_SOURCE_GENERATOR "TGZ;ZIP")
set (CPACK_SOURCE_IGNORE_FILES "/\\\\.git;/build;/CMakeLists.txt.user")
set (CPACK_SOURCE_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}")

include (CPack)

