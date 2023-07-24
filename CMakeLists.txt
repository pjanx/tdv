cmake_minimum_required (VERSION 3.0)
project (tdv VERSION 0.1.0 LANGUAGES C)

# Adjust warnings
if ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUCC)
	set (ignores "-Wno-missing-field-initializers -Wno-cast-function-type")
	set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99 ${ignores}")
	set (CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -Wall -Wextra")
endif ()

add_definitions (-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_38)

# For custom modules
set (CMAKE_MODULE_PATH
	"${PROJECT_SOURCE_DIR}/cmake;${PROJECT_SOURCE_DIR}/liberty/cmake")

# Cross-compilation for Windows, as a proof-of-concept pulled in from logdiag
if (WIN32)
	if (NOT CMAKE_CROSSCOMPILING)
		message (FATAL_ERROR "Win32 must be cross-compiled to build sensibly")
	endif ()

	set (win32_deps_root "${PROJECT_SOURCE_DIR}")
	set (win32_deps_prefix "${win32_deps_root}/mingw64")
	list (APPEND CMAKE_PREFIX_PATH "${win32_deps_prefix}")
	list (APPEND CMAKE_INCLUDE_PATH "${win32_deps_prefix}/lib")
	set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mms-bitfields")

	if (CMAKE_CROSSCOMPILING)
		list (APPEND CMAKE_FIND_ROOT_PATH ${win32_deps_prefix})
	endif ()

	# Relativize prefixes, and bar pkg-config from looking up host libraries
	set (ENV{PKG_CONFIG_SYSROOT_DIR} "${win32_deps_root}")
	set (win32_deps_pcpath
		"${win32_deps_prefix}/share/pkgconfig:${win32_deps_prefix}/lib/pkgconfig")
	set (ENV{PKG_CONFIG_PATH} "${win32_deps_pcpath}")
	set (ENV{PKG_CONFIG_LIBDIR} "${win32_deps_pcpath}")
endif ()

# Dependencies
find_package (ZLIB REQUIRED)
find_package (Ncursesw REQUIRED)
find_package (PkgConfig REQUIRED)
pkg_check_modules (dependencies REQUIRED glib-2.0>=2.38 gio-2.0 pango)

pkg_check_modules (icu icu-uc icu-i18n)
if (NOT icu_FOUND AND NOT WIN32)
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
	# We don't want the library to install, but EXCLUDE_FROM_ALL ignores tests
	add_subdirectory (termo EXCLUDE_FROM_ALL)
	file (WRITE ${PROJECT_BINARY_DIR}/CTestCustom.cmake
		"execute_process (COMMAND ${CMAKE_COMMAND} --build termo)")

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

	include_directories (${xcb_INCLUDE_DIRS})
	link_directories (${xcb_LIBRARY_DIRS})
endif ()

pkg_check_modules (gtk gtk+-3.0)
option (WITH_GUI "Build an alternative GTK+ UI" ${gtk_FOUND})
if (WITH_GUI)
	if (NOT gtk_FOUND)
		message (FATAL_ERROR "GTK+ not found")
	endif ()

	include_directories (${gtk_INCLUDE_DIRS})
	link_directories (${gtk_LIBRARY_DIRS})
endif ()

link_directories (${dependencies_LIBRARY_DIRS} ${icu_LIBRARY_DIRS})
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
find_program (A2X_EXECUTABLE a2x)
if (NOT ASCIIDOCTOR_EXECUTABLE AND NOT A2X_EXECUTABLE)
	message (WARNING "Neither asciidoctor nor a2x were found, "
		"falling back to a substandard manual page generator")
endif ()

foreach (page "${PROJECT_NAME}.1")
	set (page_output "${PROJECT_BINARY_DIR}/${page}")
	list (APPEND project_MAN_PAGES "${page_output}")
	if (ASCIIDOCTOR_EXECUTABLE)
		add_custom_command (OUTPUT ${page_output}
			COMMAND ${ASCIIDOCTOR_EXECUTABLE} -b manpage
				-a release-version=${PROJECT_VERSION}
				-o "${page_output}"
				"${PROJECT_SOURCE_DIR}/docs/${page}.adoc"
			DEPENDS "docs/${page}.adoc"
			COMMENT "Generating man page for ${page}" VERBATIM)
	elseif (A2X_EXECUTABLE)
		add_custom_command (OUTPUT ${page_output}
			COMMAND ${A2X_EXECUTABLE} --doctype manpage --format manpage
				-a release-version=${PROJECT_VERSION}
				-D "${PROJECT_BINARY_DIR}"
				"${PROJECT_SOURCE_DIR}/docs/${page}.adoc"
			DEPENDS "docs/${page}.adoc"
			COMMENT "Generating man page for ${page}" VERBATIM)
	else ()
		set (ASCIIMAN ${PROJECT_SOURCE_DIR}/liberty/tools/asciiman.awk)
		add_custom_command (OUTPUT ${page_output}
			COMMAND env LC_ALL=C asciidoc-release-version=${PROJECT_VERSION}
				awk -f ${ASCIIMAN} "${PROJECT_SOURCE_DIR}/docs/${page}.adoc"
				> ${page_output}
			DEPENDS "docs/${page}.adoc" ${ASCIIMAN}
			COMMENT "Generating man page for ${page}" VERBATIM)
	endif ()
endforeach ()

add_custom_target (docs ALL DEPENDS ${project_MAN_PAGES})

# Project libraries
set (project_common_libraries ${ZLIB_LIBRARIES} ${icu_LIBRARIES}
	${dependencies_LIBRARIES})
if (WIN32)
	find_package (LibIntl REQUIRED)
	list (APPEND project_common_libraries ${LibIntl_LIBRARIES})
endif (WIN32)

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

# Icon generation utilities
if (NOT ${CMAKE_VERSION} VERSION_LESS 3.18.0)
	set (find_program_REQUIRE REQUIRED)
endif ()

function (icon_to_png svg size output_dir output)
	set (_dimensions ${size}x${size})
	set (_png_path ${output_dir}/hicolor/${_dimensions}/apps)
	set (_png ${_png_path}/${PROJECT_NAME}.png)
	set (${output} ${_png} PARENT_SCOPE)

	find_program (rsvg_convert_EXECUTABLE rsvg-convert ${find_program_REQUIRE})
	add_custom_command (OUTPUT ${_png}
		COMMAND ${CMAKE_COMMAND} -E make_directory ${_png_path}
		COMMAND ${rsvg_convert_EXECUTABLE} --output=${_png}
			--width=${size} --height=${size} ${svg}
		DEPENDS ${svg}
		COMMENT "Generating ${_dimensions} application icon" VERBATIM)
endfunction ()

function (icon_for_win32 pngs ico)
	find_program (icotool_EXECUTABLE icotool ${find_program_REQUIRE})
	add_custom_command (OUTPUT ${ico}
		COMMAND ${icotool_EXECUTABLE} -c -o ${ico} ${pngs}
		DEPENDS ${pngs}
		COMMENT "Generating Windows program icon" VERBATIM)
endfunction ()

# Build the main executable and link it
set (project_libraries
	${project_common_libraries})
set (project_sources
	${project_common_sources}
	src/${PROJECT_NAME}.c)
set (project_headers
	${project_common_headers})

if (WITH_GUI)
	set (icon_svg ${PROJECT_SOURCE_DIR}/${PROJECT_NAME}.svg)
	set (icon_base ${PROJECT_BINARY_DIR}/icons)

	# The largest size is mainly for an appropriately sized Windows icon
	set (icon_png_list)
	foreach (icon_size 16 32 48 256)
		icon_to_png (${icon_svg} ${icon_size} ${icon_base} icon_png)
		list (APPEND icon_png_list ${icon_png})
	endforeach ()
	add_custom_target (icons ALL DEPENDS ${icon_png_list})
endif ()

if (WIN32)
	set (icon_ico ${PROJECT_BINARY_DIR}/${PROJECT_NAME}.ico)
	icon_for_win32 ("${icon_png_list}" ${icon_ico})

	set (resource_file ${PROJECT_BINARY_DIR}/${PROJECT_NAME}.rc)
	list (APPEND project_sources ${resource_file})
	add_custom_command (OUTPUT ${resource_file}
		COMMAND ${CMAKE_COMMAND} -E echo "1 ICON \"${PROJECT_NAME}.ico\""
			> ${resource_file} VERBATIM)
	set_property (SOURCE ${resource_file}
		APPEND PROPERTY OBJECT_DEPENDS ${icon_ico})
else ()
	list (APPEND project_libraries ${Ncursesw_LIBRARIES} termo-static)
	list (APPEND project_sources
		src/${PROJECT_NAME}-tui.c)
endif ()

if (WITH_X11)
	list (APPEND project_libraries ${xcb_LIBRARIES})
endif ()
if (WITH_GUI)
	list (APPEND project_libraries ${gtk_LIBRARIES})
	list (APPEND project_sources
		src/${PROJECT_NAME}-gui.c
		src/stardict-view.c)

	add_executable (${PROJECT_NAME} WIN32 ${project_sources} ${project_headers})
else ()
	add_executable (${PROJECT_NAME} ${project_sources} ${project_headers})
endif ()

target_link_libraries (${PROJECT_NAME} ${project_libraries})

# Tools
set (tools tdv-tabfile tdv-add-pronunciation tdv-query-tool tdv-transform)
foreach (tool ${tools})
	add_executable (${tool} EXCLUDE_FROM_ALL
		src/${tool}.c ${project_common_sources})
	target_link_libraries (${tool} ${project_common_libraries})
endforeach ()

option (WITH_TOOLS "Build and install some StarDict tools" ${UNIX})
if (WITH_TOOLS)
	add_custom_target (tools ALL DEPENDS ${tools})
endif ()

# Example dictionaries
file (GLOB dicts_scripts "${PROJECT_SOURCE_DIR}/dicts/*.*")
set (dicts_targets)
foreach (dict_script ${dicts_scripts})
	get_filename_component (dict_name "${dict_script}" NAME_WE)
	list (APPEND dicts_targets "dicts-${dict_name}")
	add_custom_target (dicts-${dict_name}
		COMMAND sh -c "PATH=.:$PATH \"$0\"" "${dict_script}"
		DEPENDS tdv-tabfile
		COMMENT "Generating sample dictionary ${dict_name}"
		VERBATIM)
endforeach ()

add_custom_target (dicts DEPENDS ${dicts_targets})

# The files to be installed
if (NOT WIN32)
	include (GNUInstallDirs)
	install (TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
	install (FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})

	if (WITH_TOOLS)
		install (TARGETS ${tools} DESTINATION ${CMAKE_INSTALL_BINDIR})
	endif ()
	if (WITH_GUI)
		install (FILES ${PROJECT_NAME}.svg
			DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
		install (DIRECTORY ${PROJECT_BINARY_DIR}/icons
			DESTINATION ${CMAKE_INSTALL_DATADIR})
		install (FILES ${PROJECT_NAME}.desktop
			DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
		install (FILES ${PROJECT_NAME}.xml
			DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
	endif ()

	foreach (page ${project_MAN_PAGES})
		string (REGEX MATCH "\\.([0-9])$" manpage_suffix "${page}")
		install (FILES "${page}"
			DESTINATION "${CMAKE_INSTALL_MANDIR}/man${CMAKE_MATCH_1}")
	endforeach ()
elseif (WITH_GUI)
	# This rather crude filter has been mostly copied over from logdiag
	install (TARGETS ${PROJECT_NAME} DESTINATION .)
	install (DIRECTORY
		${win32_deps_prefix}/bin/
		DESTINATION .
		FILES_MATCHING PATTERN "*.dll")
	install (DIRECTORY
		${win32_deps_prefix}/etc/
		DESTINATION etc)
	install (DIRECTORY
		${win32_deps_prefix}/lib/gdk-pixbuf-2.0
		DESTINATION lib
		FILES_MATCHING PATTERN "*" PATTERN "*.a" EXCLUDE)
	install (DIRECTORY
		${win32_deps_prefix}/share/glib-2.0/schemas
		DESTINATION share/glib-2.0)

	install (DIRECTORY
		${win32_deps_prefix}/share/icons/Adwaita
		DESTINATION share/icons OPTIONAL)
	install (FILES
		${win32_deps_prefix}/share/icons/hicolor/index.theme
		DESTINATION share/icons/hicolor)
	install (DIRECTORY ${icon_base} DESTINATION share)

	install (SCRIPT cmake/Win32Cleanup.cmake)

	find_program (GTK_UPDATE_ICON_CACHE_EXECUTABLE gtk-update-icon-cache)
	if (NOT GTK_UPDATE_ICON_CACHE_EXECUTABLE)
		message (FATAL_ERROR "gtk-update-icon-cache not found")
	endif ()

	install (CODE "execute_process (COMMAND
		sh \"${PROJECT_SOURCE_DIR}/cmake/Win32CleanupAdwaita.sh\"
		WORKING_DIRECTORY \${CMAKE_INSTALL_PREFIX})")
	install (CODE " # This may speed up program start-up a little bit
		execute_process (COMMAND \"${GTK_UPDATE_ICON_CACHE_EXECUTABLE}\"
			\"\${CMAKE_INSTALL_PREFIX}/share/icons/Adwaita\")")
endif ()

# Do some unit tests
option (BUILD_TESTING "Build tests" OFF)
if (BUILD_TESTING)
	enable_testing ()

	find_program (xmlwf_EXECUTABLE xmlwf)
	find_program (xmllint_EXECUTABLE xmllint)
	foreach (xml ${PROJECT_NAME}.xml ${PROJECT_NAME}.svg)
		if (xmlwf_EXECUTABLE)
			add_test (test-xmlwf-${xml} ${xmlwf_EXECUTABLE}
				${PROJECT_SOURCE_DIR}/${xml})
		endif ()
		if (xmllint_EXECUTABLE)
			add_test (test-xmllint-${xml} ${xmllint_EXECUTABLE} --noout
				${PROJECT_SOURCE_DIR}/${xml})
		endif ()
	endforeach ()

	find_program (dfv_EXECUTABLE desktop-file-validate)
	if (dfv_EXECUTABLE)
		foreach (df ${PROJECT_NAME}.desktop)
			add_test (test-dfv-${df} ${dfv_EXECUTABLE}
				${PROJECT_SOURCE_DIR}/${df})
		endforeach ()
	endif ()

	foreach (name stardict)
		add_executable (test-${name}
			src/test-${name}.c ${project_common_sources})
		target_link_libraries (test-${name} ${project_common_libraries})
		add_test (test-${name} test-${name})
	endforeach ()
endif ()

# CPack
set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "Translation dictionary viewer")
set (CPACK_PACKAGE_VENDOR "Premysl Eric Janouch")
set (CPACK_PACKAGE_CONTACT "Přemysl Eric Janouch <p@janouch.name>")
set (CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set (CPACK_GENERATOR "TGZ;ZIP")
set (CPACK_PACKAGE_FILE_NAME
	"${PROJECT_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set (CPACK_PACKAGE_INSTALL_DIRECTORY "${PROJECT_NAME} ${PROJECT_VERSION}")
set (CPACK_SOURCE_GENERATOR "TGZ;ZIP")
set (CPACK_SOURCE_IGNORE_FILES "/\\\\.git;/build;/CMakeLists.txt.user")
set (CPACK_SOURCE_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}")

# XXX: It is still possible to install multiple copies, making commands collide.
set (CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set (CPACK_PACKAGE_INSTALL_REGISTRY_KEY "${PROJECT_NAME}")

set (CPACK_NSIS_INSTALLED_ICON_NAME ${PROJECT_NAME}.exe)
set (CPACK_PACKAGE_EXECUTABLES ${PROJECT_NAME} ${PROJECT_NAME})
set (CPACK_NSIS_EXECUTABLES_DIRECTORY .)
set (CPACK_NSIS_EXTRA_INSTALL_COMMANDS [[
	WriteRegStr HKCR '.ifo' '' 'tdv.Dictionary'
	WriteRegStr HKCR 'tdv.Dictionary' '' 'StarDict Dictionary'
	WriteRegStr HKCR 'tdv.Dictionary\\shell\\open\\command' '' '\"$INSTDIR\\tdv.exe\" \"%1\"'
	System::Call 'shell32::SHChangeNotify(i,i,i,i) (0x08000000, 0x1000, 0, 0)'
]])
set (CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS [[
	DeleteRegKey HKCR 'tdv.Dictionary'
	System::Call 'shell32::SHChangeNotify(i,i,i,i) (0x08000000, 0x1000, 0, 0)'
]])

include (CPack)
