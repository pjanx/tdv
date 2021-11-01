#!/bin/sh -e
# Removes unused icons from the Adwaita theme, it could be even more aggressive,
# since it keeps around lots of sizes and all the GTK+ stock icons
export LC_ALL=C
find share/icons/Adwaita -type f | awk 'BEGIN {
	while (("grep -aho \"[a-z][a-z-]*\" *.dll *.exe" | getline) > 0)
		good[$0] = 1
} /[.](png|svg|cur|ani)$/ {
	# Cut out the basename without extensions
	match($0, /[^\/]+$/)
	base = substr($0, RSTART)
	sub(/[.].+$/, "", base)

	# Try matching while cutting off suffixes
	# Disregarding the not-much-used GTK_ICON_LOOKUP_GENERIC_FALLBACK
	while (!(keep = good[base]) &&
		sub(/-(ltr|rtl|symbolic)$/, "", base)) {}
	if (!keep)
		print
}' | xargs rm
