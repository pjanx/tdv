#!/bin/sh -e
# GNU/FDL English-Czech dictionary, see https://www.svobodneslovniky.cz/
curl -Lo- https://www.svobodneslovniky.cz/data/en-cs.txt.gz | \
zcat | grep -v ^# | sed 's/\\//g' | perl -CSD -F\\t -le '
	sub e { shift =~ s/\\/\\\\/gr =~ s/\n/\\n/gr =~ s/\t/\\t/gr }
	sub w {
		open(my $f, "|-", "tabfile gnu-fdl-$_[0]") or die $!;
		print $f e($k) . "\t" . e(join("\n", @$v))
			while ($k, $v) = each %{$_[1]};
		close($f);
	}
	my ($en, $cz, $notes, $special, $translator) = @F;
	if ($cz) {
		$notes =~ s/\w+:\s?//g;          # remove word classes
		$notes =~ s/(\w+\.)(?!])/($1)/;  # quote "pl."
		push(@{$encz{$en}}, $notes ? "$cz " . $notes : $cz);
		push(@{$czen{$cz}}, $notes ? "$en " . $notes : $en);
	} END {
		w("en-cz", \%encz);
		w("cz-en", \%czen);
	}'
