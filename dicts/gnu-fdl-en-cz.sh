#!/bin/sh -e
# GNU/FDL English-Czech dictionary, see https://www.svobodneslovniky.cz/
curl -Lo- https://www.svobodneslovniky.cz/data/en-cs.txt.gz | \
zcat | grep -v ^# | sed 's/\\//g' | perl -CSD -F\\t -le '
	sub tabesc { shift =~ s/\\/\\\\/gr =~ s/\n/\\n/gr =~ s/\t/\\t/gr }
	sub w {
		my ($name, $dict, $collation) = @_;
		open(my $f, "|-", "tabfile", "--pango", "--collation=$collation",
			"--website=https://www.svobodneslovniky.cz",
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
	my ($en, $cs, $notes, $special, $translator) = @F;
	if ($cs) {
		$notes =~ s/\w+:\s?//g;          # remove word classes
		$notes =~ s/(\w+\.)(?!])/($1)/;  # quote "pl."
		push(@{$encs{$en}}, entry($cs, $notes));
		push(@{$csen{$cs}}, entry($en, $notes));
	} END {
		w("en-cz", \%encs, "en");
		w("cz-en", \%csen, "cs");
	}'
