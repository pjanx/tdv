#!/bin/sh -e
# Win64Depends.sh: download dependencies from MSYS2 for cross-compilation.
# Dependencies: AWK, sed, sha256sum, cURL, bsdtar, wine64
repository=https://repo.msys2.org/mingw/mingw64/

status() {
	echo "$(tput bold)-- $*$(tput sgr0)"
}

dbsync() {
	status Fetching repository DB
	[ -f db.tsv ] || curl -# "$repository/mingw64.db" | bsdtar -xOf- | awk '
		function flush() { print f["%NAME%"] f["%FILENAME%"] f["%DEPENDS%"] }
		NR > 1 && $0 == "%FILENAME%" { flush(); for (i in f) delete f[i] }
		!/^[^%]/ { field = $0; next } { f[field] = f[field] $0 "\t" }
		field == "%SHA256SUM%" { path = "*packages/" f["%FILENAME%"]
			sub(/\t$/, "", path); print $0, path > "db.sums" } END { flush() }
	' > db.tsv
}

fetch() {
	status Resolving "$@"
	mkdir -p packages
	awk -F'\t' 'function get(name,    i, a) {
		if (visited[name]++ || !(name in filenames)) return
		print filenames[name]; split(deps[name], a); for (i in a) get(a[i])
	} BEGIN { while ((getline < "db.tsv") > 0) {
		filenames[$1] = $2; deps[$1] = ""; for (i = 3; i <= NF; i++) {
			gsub(/[<=>].*/, "", $i); deps[$1] = deps[$1] $i FS }
	} for (i = 0; i < ARGC; i++) get(ARGV[i]) }' "$@" | while IFS= read -r name
	do
		status Fetching "$name"
		[ -f "packages/$name" ] || curl -#o "packages/$name" "$repository/$name"
	done
}

verify() {
	status Verifying checksums
	sha256sum --ignore-missing --quiet -c db.sums
}

extract() {
	status Extracting packages
	for subdir in *
	do [ -d "$subdir" -a "$subdir" != packages ] && rm -rf -- "$subdir"
	done
	for i in packages/*
	do bsdtar -xf "$i" --strip-components 1 mingw64
	done
}

configure() {
	status Configuring packages
	glib-compile-schemas share/glib-2.0/schemas
	wine64 bin/gdk-pixbuf-query-loaders.exe \
		> lib/gdk-pixbuf-2.0/2.10.0/loaders.cache
}

# This directory name matches the prefix in .pc files, so we don't need to
# modify them (pkgconf has --prefix-variable, but CMake can't pass that option).
mkdir -p mingw64
cd mingw64
dbsync
fetch mingw-w64-x86_64-gtk3 mingw-w64-x86_64-icu \
	mingw-w64-x86_64-libwinpthread-git # Because we don't do "provides"?
verify
extract
configure

status Success

# XXX: Why is this override needed to run some GLib-based things under wine64?
unset XDG_DATA_DIRS
