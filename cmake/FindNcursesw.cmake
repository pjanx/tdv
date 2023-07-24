# Public Domain

find_package (PkgConfig REQUIRED)
pkg_check_modules (Ncursesw QUIET ncursesw)

# OpenBSD doesn't provide a pkg-config file
set (required_vars Ncursesw_LIBRARIES)
if (NOT Ncursesw_FOUND)
	find_library (Ncursesw_LIBRARIES NAMES ncursesw)
	find_path (Ncursesw_INCLUDE_DIRS ncurses.h PATH_SUFFIXES ncurses)
	list (APPEND required_vars Ncursesw_INCLUDE_DIRS)
endif (NOT Ncursesw_FOUND)

include (FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS (Ncursesw DEFAULT_MSG ${required_vars})

mark_as_advanced (Ncursesw_LIBRARIES Ncursesw_INCLUDE_DIRS)
