#!/bin/sh -e
# This shell script generates the translation template.
#
# The reason for this not being inside CMakeLists.txt is that the translator
# should not need to run the whole configuration process just to get this file.
dir=$(dirname $0)

re='^[ \t]*project *( *\([^ \t)]\{1,\}\) \{1,\}VERSION \{1,\}\([^ \t)]\{1,\}\).*'
package=$(sed -n "s/$re/\\1/p" "$dir/../CMakeLists.txt")
version=$(sed -n "s/$re/\\2/p" "$dir/../CMakeLists.txt")
if [ -z "$package" -o -z "$version" ]; then
	echo "Failed to get information from CMakeLists.txt"
	exit 1
fi

xgettext -LC -k_ -kN_ $dir/../src/*.c -o "$dir/$package.pot" \
	--package-name="$package" --package-version="$version" \
	--copyright-holder="Přemysl Eric Janouch" \
	--msgid-bugs-address="https://git.janouch.name/p/$package/issues"
