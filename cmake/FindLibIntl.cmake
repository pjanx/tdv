# Public Domain

find_library (LibIntl_LIBRARIES intl)

include (FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS (LibIntl DEFAULT_MSG LibIntl_LIBRARIES)

mark_as_advanced (LibIntl_LIBRARIES)
