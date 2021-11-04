#!/usr/bin/env perl
# Czech WordNet 1.9 PDT, CC BY-NC-SA 3.0, newer versions available commercially;
# this one's IDs cannot be linked with any release of the Princeton WordNet
use warnings;
use strict;

# GNU Gzip can unpack a ZIP file, but not the BSD one, and unzip can't use stdin
my $zipcat = qx/command -v bsdtar/ ? 'bsdtar -xOf-' : 'zcat';

my $base = 'https://lindat.cz/repository/xmlui';
my $path = 'handle/11858/00-097C-0000-0001-4880-3';
open(my $doc, '-|',
	"curl -Lo- '$base/bitstream/$path/Czech_WordNet_1.9_PDT.zip'"
		. " | $zipcat | iconv -f latin2 -t UTF-8") or die $!;

# https://nlp.fi.muni.cz/trac/deb2/wiki/WordNetFormat but not quite;
# for terminology see https://wordnet.princeton.edu/documentation/wngloss7wn
my %synsets;
while (<$doc>) {
	my $id = m|<ID>(.+?)</ID>| && $1; next unless defined $id;
	my $pos = m|<POS>(.+?)</POS>| && $1; next if $pos eq 'e';
	$synsets{$id} = {
		literals => [map {s| \^\d+||gr} m|<LITERAL>(.+?)<|g],
		rels => {
			anto => [m^<ILR>(.+?)<TYPE>near_antonym<^g],
			hyper => [m^<ILR>(.+?)<TYPE>hypernym<^g],
			hypo => [m^<ILR>(.+?)<TYPE>hyponym<^g,
				m^<SUBEVENT>(.+?)</SUBEVENT>^g],
			super => [m^<ILR>(.+?)<TYPE>holo_part<^g],
			sub => [m^<ILR>(.+?)<TYPE>(?:holo_member|mero_part|partonym)<^g],
		},
	};
}

# Resolve all synset links to hash references, filtering out what can't be found
while (my ($id, $synset) = each %synsets) {
	while (my ($name, $links) = each %{$synset->{rels}}) {
		@$links = map {$synsets{$_} || ()} @$links;
	}
}

# Ensure symmetry in relationships, duplicates will be taken care of later
my %antitags = qw(anto anto hyper hypo hypo hyper super sub sub super);
while (my ($id, $synset) = each %synsets) {
	while (my ($name, $links) = each %{$synset->{rels}}) {
		push @{$_->{rels}->{$antitags{$name}}}, $synset for @$links;
	}
}

# Create an inverse index from literals/keywords to their synsets
my %literals;
while (my ($id, $synset) = each %synsets) {
	push @{$literals{$_}}, $synset for @{$synset->{literals}};
}

# Output synsets exploded to individual words, with expanded relationships
close($doc) or die $?;
open(my $tabfile, '|-', 'tabfile', 'czech-wordnet',
	'--book-name=Czech WordNet 1.9 PDT', "--website=$base/$path",
	'--date=2011-01-24', '--collation=cs_CZ') or die $!;

sub expand {
	my %seen;
	return grep {!$seen{$_}++} (map {@{$_->{literals}}} @_);
}

for my $keyword (sort {lc $a cmp lc $b} keys %literals) {
	my @lines;
	for my $synset (@{$literals{$keyword}}) {
		my $rels = $synset->{rels};
		push @lines,
			(grep {$_ ne $keyword} @{$synset->{literals}}),
			(map {"$_ ↑"} expand(@{$rels->{hyper}})),
			(map {"$_ ↓"} expand(@{$rels->{hypo}})),
			(map {"$_ ⊃"} expand(@{$rels->{super}})),
			(map {"$_ ⊂"} expand(@{$rels->{sub}})),
			(map {"$_ ≠"} expand(@{$rels->{anto}}));
	}
	if (@lines) {
		print $tabfile "$keyword\t" . join('\n',
			map { s/&lt;/</gr =~ s/&gt;/>/gr =~ s/&amp;/&/gr
				=~ s/\\/\\\\/gr =~ s/\n/\\n/gr =~ s/\t/\\t/gr} @lines) . "\n";
	}
}
close($tabfile) or die $?;
