#!/bin/sh -e
# Slovník cizích slov, see https://slovnik-cizich-slov.abz.cz/web.php/o-slovniku
# TODO: Skipping the optional pronunciation field, tabfile can't handle it yet,
# but could be made to accept a lowercase sametypesequence
curl -Lo- https://slovnik-cizich-slov.abz.cz/export.php | \
iconv -f latin2 -t UTF-8 | perl -CSD -F\\\| -le '
	print "$_\t" . $F[2] =~ s/\\/\\\\/gr =~ s/; /\\n/gr for split(", ", $F[0])
' | sort -u | tabfile slovnik-cizich-slov
