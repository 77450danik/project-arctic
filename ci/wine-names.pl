#!/usr/bin/perl
# Wine's name off what a person sees of Arctic. Run by ci/build-wine.sh in
# the staged tree, after Arctic's modules and patches are in:
#
# - the registry keys Software\Wine of HKLM and HKCU (and their 32-bit
#   views) are Software\Arctic\Runtime, WineDbg in them Debugger. A registry
#   made before has its keys moved once by arctic-init (rename_wine_keys);
# - a file's version: "Arctic" as the product where it said "Wine 11.18",
#   "Arctic <module>" as the description where it said "Wine <module>".
#   The copyright stays as it is: the code is the Wine authors';
# - the HID, USB and Bluetooth buses in the Device Manager: Arctic's as the
#   maker, and the devices are named as Windows names them ("HID-compliant
#   device", "XINPUT compatible HID device");
# - the voice of speech: Arctic Default Voice.
#
# Names a program never shows stay (winebus.sys, the WINEBUS bus, the
# WinePlugPlay group, IWine* interfaces): the drivers and the 32/64-bit
# marshalling find each other by them.
use strict;
use warnings;
use File::Find;

my $tree = shift // '.';
chdir $tree or die "$tree: $!";

sub edit {
    my ($file, $code) = @_;
    open my $in, '<:raw', $file or die "$file: $!";
    local $/;
    my $text = <$in>;
    close $in;
    local $_ = $text;
    $code->();
    return 0 if $_ eq $text;
    open my $out, '>:raw', $file or die "$file: $!";
    print $out $_;
    close $out;
    return 1;
}

# Software\Wine in every form the sources write it: "Software\\Wine" in C,
# Software\Wine in .inf, the 32-bit view's Wow6432Node\Wine, and the WCHAR
# arrays {'S','o','f','t','w','a','r','e','\\','W','i','n','e',...}
my @sw = map { "'$_'" } split //, 'Software';
my $array_old = join(',\s*', map { quotemeta } @sw, q{'\\\\'}, map { "'$_'" } split //, 'Wine');
my $array_new = join(',', @sw, q{'\\\\'}, (map { "'$_'" } split //, 'Arctic'), q{'\\\\'},
                     map { "'$_'" } split //, 'Runtime');

sub registry {
    s{(Software)(\\+)Wine\2WineDbg(?![\w])}{$1$2Arctic$2Runtime$2Debugger}gi;
    s{(Software)(\\+)((?:Wow6432Node\2)?)Wine(?![\w])}{$1$2$3Arctic$2Runtime}gi;
    s/$array_old(?=\s*[,}])/$array_new/g;
}

my ($keys, $files) = (0, 0);
find({ no_chdir => 1, wanted => sub {
    if (-d $_) {
        $File::Find::prune = 1 if m{/(\.git|tests|autom4te\.cache)$};
        return;
    }
    return unless /\.(c|h|in|inf|rc|rgs|reg|idl|l|y)$/;
    $files++;
    $keys++ if edit($_, \&registry);
}}, '.');
print "wine-names: Software\\Wine is Software\\Arctic\\Runtime in $keys of $files files\n";

# The product: "@PACKAGE_STRING@" was "Wine 11.18"
edit('include/ntverp.h.in', sub {
    s{(#define VER_PRODUCTNAME_STR )"\@PACKAGE_STRING\@"}{$1"Arctic"} or die "ntverp.h.in: no product name\n";
});
# The description of a module that names none: "Wine <module>"
edit('tools/makedep.c', sub {
    s{strmake\( "\\"Wine %s\\"", get_basename_noext}{strmake( "\\"Arctic %s\\"", get_basename_noext}
        or die "makedep.c: no default description\n";
});

# The descriptions modules name: in Makefile.in, and in the .rc of the few
# that translate theirs, with the translation in po/ under the same msgid
my %msgid;
find({ no_chdir => 1, wanted => sub {
    if (-d $_) {
        $File::Find::prune = 1 if m{/(\.git|tests)$};
        return;
    }
    if (/\/Makefile\.in$/) {
        edit($_, sub { s{^(VER_FILEDESCRIPTION_STR\s*=.*)$}{(my $l = $1) =~ s/\bWine\b/Arctic/g; $l}gme });
    } elsif (/\.rc$/) {
        edit($_, sub {
            s{^(#define VER_FILEDESCRIPTION_STR )"([^"]*\bWine\b[^"]*)"}{
                my ($define, $old) = ($1, $2);
                (my $new = $old) =~ s/\bWine\b/Arctic/g;
                $msgid{$old} = $new;
                "$define\"$new\"";
            }gme;
        });
    }
}}, 'dlls', 'programs');
for my $po (glob 'po/*.po') {
    edit($po, sub {
        my @out;
        my $in_entry = 0;
        for my $line (split /(?<=\n)/) {
            if ($line =~ /^msgid "(.*)"$/) {
                $in_entry = exists $msgid{$1};
                $line = "msgid \"$msgid{$1}\"\n" if $in_entry;
            } elsif ($in_entry && $line =~ /^(msgstr )?"/) {
                $line =~ s/\bWine\b/Arctic/g;
            } else {
                $in_entry = 0;
            }
            push @out, $line;
        }
        $_ = join '', @out;
    });
}

# The voice speech lists (an empty token of sapi's)
edit('dlls/sapi/sapi.rgs', sub { s{'Wine Default Voice'}{'Arctic Default Voice'} or die "sapi.rgs: no voice\n" });

# The buses' .inf: the maker and the names of the devices and services
for my $inf (glob 'dlls/{winebus,winehid,wineusb,winebth,winexinput,hidclass,mouhid}.sys/*.inf') {
    edit($inf, sub {
        s{^Wine HID compatible device=}{HID Input Device=}m;
        s{^Wine HID device=}{HID-compliant device=}m;
        s{^Wine XInput compatible device=}{XINPUT compatible HID device=}m;
        s{^Wine=}{Arctic=}m;
        s{\bWine\b(?= (HID|USB|Bluetooth|XInput))}{Arctic}g;
    });
}
