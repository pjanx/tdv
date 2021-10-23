# To be run from cmake_install.cmake, eradicates all unreferenced libraries.
# CMake 3.9.6 has a parsing bug with ENCODING UTF-8.
cmake_minimum_required (VERSION 3.10)

# CPack runs this almost without any CMake variables at all
# (cmStateSnapshot::SetDefaultDefinitions(), CMAKE_INSTALL_PREFIX, [DESTDIR])
set (installdir "${CMAKE_INSTALL_PREFIX}")
if (NOT installdir OR installdir MATCHES "^/usr(/|$)")
	return ()
endif ()

# The function is recursive and CMake has tragic scoping behaviour;
# environment variables are truly global there, in the absence of a cache
unset (ENV{seen})
function (expand path)
	set (seen $ENV{seen})
	if (path IN_LIST seen OR NOT EXISTS "${path}")
		return ()
	endif ()

	set (ENV{seen} "$ENV{seen};${path}")
	file (STRINGS "${path}" strings REGEX "[.][Dd][Ll][Ll]$" ENCODING UTF-8)
	foreach (string ${strings})
		string (REGEX MATCH "[-.+_a-zA-Z0-9]+$" word "${string}")
		expand ("${installdir}/${word}")
	endforeach ()
endfunction ()

file (GLOB roots LIST_DIRECTORIES false "${installdir}/*.[Ee][Xx][Ee]"
	"${installdir}/lib/gdk-pixbuf-2.0/2.10.0/loaders/*.[Dd][Ll][Ll]")
foreach (binary ${roots})
	expand ("${binary}")
endforeach ()

file (GLOB libraries LIST_DIRECTORIES false "${installdir}/*.[Dd][Ll][Ll]")
list (REMOVE_ITEM libraries $ENV{seen})
file (REMOVE ${libraries})
