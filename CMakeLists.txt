project (sdtui C)
cmake_minimum_required (VERSION 2.8.5)

# Moar warnings
if ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUC)
	set (CMAKE_C_FLAGS "-std=gnu99")
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

# Configuration
include (CheckFunctionExists)
CHECK_FUNCTION_EXISTS ("wcwidth" HAVE_WCWIDTH)

# Dependecies
find_package (ZLIB REQUIRED)

find_package (PkgConfig REQUIRED)
pkg_check_modules (dependencies REQUIRED ncursesw glib-2.0 gio-2.0 pango)

include_directories (${ZLIB_INCLUDE_DIRS} ${dependencies_INCLUDE_DIRS})

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
set (project_common_libraries ${ZLIB_LIBRARIES} ${dependencies_LIBRARIES})

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
	string (REGEX MATCH "\\.([0-9])" manpage_suffix "${page}")
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

