#!/bin/sh
# This shell script generates the translation template.
#
# The reason for this not being inside CMakeLists.txt
# is that the translator should not need to run the whole
# configuration process just to get this single stupid file.

# Get the directory this script resides in so that the user
# doesn't have to run the script from there
DIR=$(dirname $0)

# Collect source files
SOURCES=$(echo $DIR/../src/*.c)

# Get the package name from CMakeLists.txt
PACKAGE=$(sed -n '/^[ \t]*project[ \t]*([ \t]*\([^ \t)]\{1,\}\).*).*/{s//\1/p;q}' \
	$DIR/../CMakeLists.txt)

# Get the package version from CMakeLists.txt
EXP_BEG='/^[ \t]*set[ \t]*([ \t]*project_VERSION_'
EXP_END='[ \t]\{1,\}"\{0,1\}\([^)"]\{1,\}\)"\{0,1\}).*/{s//\1/p;q}'

MAJOR=$(sed -n "${EXP_BEG}MAJOR${EXP_END}" $DIR/../CMakeLists.txt)
MINOR=$(sed -n "${EXP_BEG}MINOR${EXP_END}" $DIR/../CMakeLists.txt)
PATCH=$(sed -n "${EXP_BEG}PATCH${EXP_END}" $DIR/../CMakeLists.txt)

if [ "$MAJOR" != "" ]; then
	VERSION=$MAJOR
	if [ "$MINOR" != "" ]; then
		VERSION=$VERSION.$MINOR
		if [ "$PATCH" != "" ]; then
			VERSION=$VERSION.$PATCH
		fi
	fi
fi

if [ -z "$PACKAGE" -o -z "$VERSION" ]; then
	echo "Failed to get information from CMakeLists.txt"
	exit 1
fi

# Finally make the template
xgettext -LC -k_ -kN_ $SOURCES -o "$DIR/$PACKAGE.pot" \
	--package-name="$PACKAGE" --package-version="$VERSION" \
	--copyright-holder="Přemysl Janouch" \
	--msgid-bugs-address="https://github.com/pjanouch/$PACKAGE/issues"

