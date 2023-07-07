#!/bin/sh -e
# GNU/FDL German-Czech dictionary, see https://gnu.nemeckoceskyslovnik.cz

# Sometimes the domain doesn't resolve, and the contents are close to useless
[ -n "$WANT_BAD_DICTS" ] || exit 0

curl -Lo- 'https://gnu.nemeckoceskyslovnik.cz/index.php?id=6&sablona=export&format=zcu' | \
grep -v ^# | sed 's/\\//g' | perl -CSD -F\\t -le '
	sub tabesc { shift =~ s/\\/\\\\/gr =~ s/\n/\\n/gr =~ s/\t/\\t/gr }
	sub w {
		my ($name, $dict, $collation) = @_;
		open(my $f, "|-", "tdv-tabfile", "--pango", "--collation=$collation",
			"--website=https://gnu.nemeckoceskyslovnik.cz",
			"gnu-fdl-$name") or die $!;
		print $f tabesc($keyword) . "\t" . tabesc(join("\n", @$defs))
			while ($keyword, $defs) = each %{$dict};
		close($f);
	}
	sub xmlesc { shift =~ s/&/&amp;/gr =~ s/</&lt;/gr =~ s/>/&gt;/gr }
	sub entry {
		my ($definition, $notes) = map {xmlesc($_)} @_;
		$notes ? "$definition <i>$notes</i>" : $definition;
	}
	next if !$_ .. 0;
	my ($de, $cs, $notes, $special, $translator) = @F;
	if ($cs) {
		$notes =~ s/\w+:\s?//g;          # remove word classes
		$notes =~ s/(\w+\.)(?!])/($1)/;  # quote "pl."
		push(@{$decs{$de}}, entry($cs, $notes));
		push(@{$csde{$cs}}, entry($de, $notes));
	} END {
		w("de-cz", \%decs, "de");
		w("cz-de", \%csde, "cs");
	}'
