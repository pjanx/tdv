project (sdtui C)
cmake_minimum_required (VERSION 2.8.5)

# Moar warnings
if ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUC)
	set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99")
	set (CMAKE_C_FLAGS_DEBUG
		"${CMAKE_C_FLAGS_DEBUG} -Wall -Wextra -Wno-missing-field-initializers")
endif ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUC)

# Version
set (project_VERSION_MAJOR "0")
set (project_VERSION_MINOR "1")
set (project_VERSION_PATCH "0")

set (project_VERSION "${project_VERSION_MAJOR}")
set (project_VERSION "${project_VERSION}.${project_VERSION_MINOR}")
set (project_VERSION "${project_VERSION}.${project_VERSION_PATCH}")

# For custom modules
set (CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

# Dependencies
find_package (ZLIB REQUIRED)
find_package (Ncursesw REQUIRED)
find_package (PkgConfig REQUIRED)
pkg_check_modules (dependencies REQUIRED glib-2.0 gio-2.0 pango)

pkg_check_modules (icu icu-uc icu-i18n)
if (NOT icu_FOUND)
	find_program (icu_CONFIG_EXECUTABLE icu-config)
	if (NOT icu_CONFIG_EXECUTABLE)
		message (FATAL_ERROR "ICU not found")
	endif (NOT icu_CONFIG_EXECUTABLE)

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
		endif (flag MATCHES "^-I(.*)")
	endforeach (flag)

	# This should suffice most of the time, don't care about the rest
endif (NOT icu_FOUND)

find_package (Termo QUIET NO_MODULE)
option (USE_SYSTEM_TERMO
	"Don't compile our own termo library, use the system one" ${Termo_FOUND})

if (USE_SYSTEM_TERMO)
	if (NOT Termo_FOUND)
		message (FATAL_ERROR "System termo library not found")
	endif (NOT Termo_FOUND)
else (USE_SYSTEM_TERMO)
	add_subdirectory (termo EXCLUDE_FROM_ALL)
	# We don't have many good choices when we don't want to install it and want
	# to support older versions of CMake; this is a relatively clean approach
	# (other possibilities: setting a variable in the parent scope, using a
	# cache variable, writing a special config file with build paths in it and
	# including it here, or setting a custom property on the targets).
	get_directory_property (Termo_INCLUDE_DIRS
		DIRECTORY termo INCLUDE_DIRECTORIES)
	set (Termo_LIBRARIES termo-static)
endif (USE_SYSTEM_TERMO)

# We actually don't care about the specific version
pkg_search_module (gtk gtk+-3.0 gtk+-2.0)
option (WITH_GTK "Compile with GTK+ support" ${gtk_FOUND})

if (WITH_GTK)
	if (NOT gtk_FOUND)
		message (FATAL_ERROR "GTK+ library not found")
	endif (NOT gtk_FOUND)

	list (APPEND dependencies_INCLUDE_DIRS ${gtk_INCLUDE_DIRS})
	list (APPEND dependencies_LIBRARY_DIRS ${gtk_LIBRARY_DIRS})
	list (APPEND dependencies_LIBRARIES ${gtk_LIBRARIES})
endif (WITH_GTK)

link_directories (${dependencies_LIBRARY_DIRS})
include_directories (${ZLIB_INCLUDE_DIRS} ${icu_INCLUDE_DIRS}
	${dependencies_INCLUDE_DIRS} ${NCURSESW_INCLUDE_DIRS}
	${Termo_INCLUDE_DIRS})

# Configuration
include (CheckFunctionExists)
set (CMAKE_REQUIRED_LIBRARIES ${NCURSESW_LIBRARIES})
CHECK_FUNCTION_EXISTS ("resizeterm" HAVE_RESIZETERM)

# Localization
find_package (Gettext REQUIRED)
file (GLOB project_PO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/po/*.po)
GETTEXT_CREATE_TRANSLATIONS (
	${CMAKE_CURRENT_SOURCE_DIR}/po/${CMAKE_PROJECT_NAME}.pot
	ALL ${project_PO_FILES})

# Documentation
find_program (XSLTPROC_EXECUTABLE xsltproc)
if (NOT XSLTPROC_EXECUTABLE)
	message (FATAL_ERROR "xsltproc not found")
endif (NOT XSLTPROC_EXECUTABLE)

set (project_MAN_PAGES "${CMAKE_PROJECT_NAME}.1")
foreach (page ${project_MAN_PAGES})
	set (page_output "${CMAKE_CURRENT_BINARY_DIR}/${page}")
	list (APPEND project_MAN_PAGES_OUTPUT "${page_output}")
	add_custom_command (OUTPUT ${page_output}
		COMMAND ${XSLTPROC_EXECUTABLE}
			--nonet
			--param make.year.ranges 1
			--param make.single.year.ranges 1
			--param man.charmap.use.subset 0
			--param man.authors.section.enabled 0
			http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl
			"${CMAKE_CURRENT_SOURCE_DIR}/docs/${page}.xml"
		DEPENDS "docs/${page}.xml"
		COMMENT "Generating man page for ${page}" VERBATIM)
endforeach (page)

add_custom_target (docs ALL DEPENDS ${project_MAN_PAGES_OUTPUT})

# Project source files
set (project_common_sources
	src/dictzip-input-stream.c
	src/generator.c
	src/stardict.c
	src/utils.c)
set (project_common_headers
	${CMAKE_CURRENT_BINARY_DIR}/config.h
	src/dictzip-input-stream.h
	src/stardict.h
	src/stardict-private.h
	src/generator.h
	src/utils.h)

# Project libraries
set (project_common_libraries ${ZLIB_LIBRARIES} ${icu_LIBRARIES}
	${dependencies_LIBRARIES} ${NCURSESW_LIBRARIES} termo-static)

# Create a common project library so that source files are only compiled once
if (${CMAKE_VERSION} VERSION_GREATER "2.8.7")
	add_library (stardict OBJECT
		${project_common_sources}
		${project_common_headers})
	set (project_common_sources $<TARGET_OBJECTS:stardict>)
else (${CMAKE_VERSION} VERSION_GREATER "2.8.7")
	add_library (stardict STATIC
		${project_common_sources}
		${project_common_headers})
	target_link_libraries (stardict ${project_common_libraries})
	list (APPEND project_common_libraries stardict)
	set (project_common_sources)
endif (${CMAKE_VERSION} VERSION_GREATER "2.8.7")

# Generate a configuration file
configure_file (${CMAKE_CURRENT_SOURCE_DIR}/config.h.in
	${CMAKE_CURRENT_BINARY_DIR}/config.h)
include_directories (${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

# Primary target source files
set (project_sources
	src/${CMAKE_PROJECT_NAME}.c)
set (project_headers
	${project_common_headers})

# Build the main executable and link it
add_executable (${CMAKE_PROJECT_NAME}
	${project_sources} ${project_headers} ${project_common_sources})
target_link_libraries (${CMAKE_PROJECT_NAME} ${project_common_libraries})

# Tools
add_executable (query-tool EXCLUDE_FROM_ALL
	src/query-tool.c ${project_common_sources})
target_link_libraries (query-tool ${project_common_libraries})

add_executable (add-pronunciation EXCLUDE_FROM_ALL
	src/add-pronunciation.c ${project_common_sources})
target_link_libraries (add-pronunciation ${project_common_libraries})

add_custom_target (tools DEPENDS add-pronunciation query-tool)

# The files to be installed
include (GNUInstallDirs)
install (TARGETS ${CMAKE_PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
install (FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})

foreach (page ${project_MAN_PAGES_OUTPUT})
	string (REGEX MATCH "\\.([0-9])$" manpage_suffix "${page}")
	install (FILES "${page}"
		DESTINATION "${CMAKE_INSTALL_MANDIR}/man${CMAKE_MATCH_1}")
endforeach (page)

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
	endforeach (name)
endif (BUILD_TESTING)

# CPack
set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "StarDict terminal UI")
set (CPACK_PACKAGE_VENDOR "Premysl Janouch")
set (CPACK_PACKAGE_CONTACT "Přemysl Janouch <p.janouch@gmail.com>")
set (CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set (CPACK_PACKAGE_VERSION_MAJOR ${project_VERSION_MAJOR})
set (CPACK_PACKAGE_VERSION_MINOR ${project_VERSION_MINOR})
set (CPACK_PACKAGE_VERSION_PATCH ${project_VERSION_PATCH})
set (CPACK_GENERATOR "TGZ;ZIP")
set (CPACK_PACKAGE_FILE_NAME
	"${CMAKE_PROJECT_NAME}-${project_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set (CPACK_PACKAGE_INSTALL_DIRECTORY "${CMAKE_PROJECT_NAME}-${project_VERSION}")
set (CPACK_SOURCE_GENERATOR "TGZ;ZIP")
set (CPACK_SOURCE_IGNORE_FILES "/\\\\.git;/build;/CMakeLists.txt.user")
set (CPACK_SOURCE_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}-${project_VERSION}")

include (CPack)

