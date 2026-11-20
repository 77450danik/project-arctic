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
# - the voice of speech: Arctic Default Voice;
# - kernelbase's Software\Wine\Time Zones is Software\Arctic\Runtime\Time
#   Zones, msdaps' WINEDB*PRX ProgIDs are DB*PRX.
#
# - the drivers and their host are Windows', not Wine's: winebus.sys,
#   winehid.sys, winebth.sys, winexinput.sys are winbus.sys, winhid.sys,
#   winbth.sys, winxinput.sys, wineusb.sys is windowsusb.sys (winusb.sys is
#   Windows' WinUSB), winedevice.exe is windevice.exe; so are their
#   services, the buses (WINBUS\, WINBTH\, WINXINPUT\, ROOT\WIN), the group
#   WinPlugPlay. So are the programs and libraries named after Wine:
#   winboot, windowsdbg (windbg is Microsoft's debugger), winbrowser,
#   winfile, winmine, winpath, winmenubuilder, winconsole, winmsibuilder,
#   wind3d, winvulkan (and its ICD json), winwayland.drv, windmo, winmapi.
#   Modules, files and every mention in the tree
#   are renamed before make_makefiles; a registry made before is renamed once
#   by arctic-init (rename_wine_keys), the old files go with the update.
#
# Names a program never shows stay (IWine* interfaces, the 32/64-bit
# marshalling finds itself by them), so do the wineps print processor and
# winealsa.drv (Drivers\winealsa.drv names the audio devices' keys). The
# host's tools keep theirs (wine, wineserver, winebuild, winegcc).
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

# kernelbase registers Software\Wine\Time Zones\TZ Mapping, the table of
# Unix zone names (Europe/Kyiv) and the Windows zones they are. ntdll looks
# the host's zone up there to name the system's zone (its WCHAR array went
# to Arctic\Runtime above), and the Date and Time page goes the other way
# with it: the table moves there too. Without it ntdll took the first zone
# whose rules matched: Warsaw was "Central Europe" (Belgrade, Prague).
edit('dlls/kernelbase/kernelbase.rgs', sub {
    s|^([ \t]*)NoRemove Wine\r?\n\1\{\r?\n(.*?)^\1\}\r?\n|${1}NoRemove Arctic\n${1}\{\n${1}    NoRemove Runtime\n${1}    \{\n${2}${1}    \}\n${1}\}\n|ms
        or die "kernelbase.rgs: no Wine key\n";
});
# msdaps' row and rowset proxies: their ProgIDs, which no code looks up
edit('dlls/msdaps/row_server.idl', sub { s{"WINEDB(ROW|RST)PRX\.}{"DB$1PRX.}g or die "row_server.idl: no ProgIDs\n" });

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

# The drivers and the process that hosts them: Wine's in their names becomes
# Windows', in the case it was written in (winebus, WineBus, WINEBUS). First
# the directories and files, deepest first, then every mention in the tree.
my $driver = qr/(wine)(bus|hid|usb|bth|xinput|device|plugplay|boot|dbg|browser|file|mine|path|menubuilder|console|msibuilder|d3d|vulkan|wayland|dmo|mapi|vdm)/i;
# "win" as Windows names its own (winmine.exe and winfile.exe were Windows'
# Minesweeper and File Manager); "windows" where "win" is a Windows part of
# another kind: windbg is Microsoft's debugger, winusb.sys the WinUSB driver;
# "nt" for the machine of 16-bit programs, ntvdm.exe in Windows
sub base_name {
    my ($word) = @_;
    return 'windows' if $word =~ /^(dbg|usb)$/i;
    return 'nt' if $word =~ /^vdm$/i;
    return 'win';
}
sub windows_case {
    my ($w, $word) = @_;
    my $base = base_name($word);
    $w eq 'WINE' ? uc $base : $w eq 'wine' ? $base : ucfirst $base;
}
my @paths;
find({ no_chdir => 1, wanted => sub {
    if (-d $_ && m{/\.git$}) { $File::Find::prune = 1; return; }
    push @paths, $_ if (split m{/})[-1] =~ $driver;
}}, '.');
for my $path (sort { ($b =~ tr{/}{}) <=> ($a =~ tr{/}{}) } @paths) {
    my ($dir, $name) = $path =~ m{^(.*)/([^/]+)$};
    (my $new = $name) =~ s/$driver/windows_case($1, $2) . $2/ge;
    rename $path, "$dir/$new" or die "$path: $!\n";
}
# the same names spelled as arrays of characters, as ntdll starts wineboot:
# {'w','i','n','e','b','o','o','t','.','e','x','e',0}
my @array_words;
for my $word (qw(bus hid usb bth xinput device plugplay boot dbg browser file mine path menubuilder
                 console msibuilder d3d vulkan wayland dmo mapi vdm)) {
    my $letters = join('', map { "\\s*,\\s*'(?i:\Q$_\E)'" } split //, $word);
    push @array_words, [$word, qr/'([wW])'\s*,\s*'([iI])'\s*,\s*'[nN]'\s*,\s*'[eE]'($letters)/];
}
sub array_name {
    my ($w, $i, $rest, $word) = @_;
    my $base = base_name($word);
    $base = uc $base if $w eq 'W' && $i eq 'I';
    $base = ucfirst $base if $w eq 'W' && $i eq 'i';
    join(',', map { "'$_'" } split //, $base) . $rest;
}
my ($mentions, $in) = (0, 0);
find({ no_chdir => 1, wanted => sub {
    if (-d $_) {
        $File::Find::prune = 1 if m{/(\.git|autom4te\.cache)$};
        return;
    }
    # json: the Vulkan ICD names its DLL; make_makefiles treats the Wayland
    # driver's directory apart; a manifest names its program's assembly
    return unless /\.(c|h|in|inf|rc|rgs|reg|idl|l|y|ac|spec|po|sh|pl|json|xml|desktop|manifest)$/ || m{^\./tools/make_makefiles$};
    my $n = 0;
    my $manifest = /\.manifest$/ ? 1 : 0;
    my $source = /\.[ch]$/ ? 1 : 0;
    $in++ if edit($_, sub {
        if ($source) {
            for my $aw (@array_words) {
                my ($word, $re) = @$aw;
                $n += s/$re/array_name($1, $2, $3, $word)/ge;
            }
        }
        $n += s/$driver/windows_case($1, $2) . $2/ge;
        $n += s/WINE_COMP_/WIN_COMP_/g;
        $n += s/(assemblyIdentity[^>]*name=")Wine\./${1}Windows./g if $manifest;
    });
    $mentions += $n;
}}, '.');
print "wine-names: drivers: ", scalar(@paths), " files and directories, $mentions mentions in $in files\n";
# a debug channel's name holds 14 characters (debug.h asserts it): one the
# rename made longer drops its prefix there; the log's channel names are no
# one's to see
my $channel = qr/((?:WINE_(?:DEFAULT|DECLARE)_DEBUG_CHANNEL|(?:TRACE|WARN|ERR|FIXME)_ON|(?:TRACE|WARN|ERR|FIXME|MESSAGE)_)\(\s*)(windows|win)(\w+)/;
find({ no_chdir => 1, wanted => sub {
    if (-d $_) { $File::Find::prune = 1 if m{/\.git$}; return; }
    return unless /\.[ch]$/;
    edit($_, sub { s/$channel/length("$2$3") > 14 ? "$1$3" : "$1$2$3"/ge });
}}, '.');
# where a length was counted by hand for the old name
edit('dlls/hidclass.sys/pnp.c', sub {   # "WINXINPUT\" is 10 characters, "WINEXIN" was 7
    s{(L"WINXINPUT\\\\",\s*)7\)}{${1}10)} or die "hidclass pnp.c: no WINXINPUT prefix\n";
});
edit('dlls/winbus.sys/main.c', sub {   # "WINBUS\VID_0000&PID_0000"
    s{(winbus_len = )25\b}{${1}24} or die "winbus main.c: no id length\n";
});
# the root devices of the buses: ROOT\WIN\WINBUS, not ROOT\WINE\...
edit('programs/winboot/winboot.c', sub {
    s{"root\\\\wine\\\\(winbth|winbus|windowsusb)"}{"root\\\\win\\\\$1"}g == 3 or die "winboot.c: not 3 root devices\n";
});
# the display driver's name is made of pieces ("wine" + "wayland" + ".drv")
# where the desktop loads it; the sound's (winealsa.drv) stays, its name is
# in the keys of the audio devices
for my $desktop ('programs/explorer/desktop.c', 'dlls/winsrv/desktop.c') {
    edit($desktop, sub { s{L"wine%s\.drv"}{L"win%s.drv"} or die "$desktop: no display driver name\n" });
}

# ---------------------------------------------------------------------------
# NT's own names (2026-11-16): nothing of the running system is named Wine.
#
#  - the server is ntexec (the NT executive), the loader ntloader (its
#    preloader ntloader-preloader), the launcher in /usr/bin ntloader too;
#    their directories /usr/lib/nt and /usr/share/nt, the setup file nt.inf,
#    the server's directory /tmp/.nt-UID. ci/build-wine.sh renames the files
#    when it installs them: the build keeps Wine's layout, only the places
#    the running code looks for them change here;
#  - the variables of the environment every program sees (`set`) and those
#    Wine's own code reads: WINEPREFIX is NTPREFIX, WINEDEBUG NTDEBUG,
#    WINEDLLOVERRIDES NTDLLOVERRIDES, WINELOADER NTLOADER, WINEDLLDIR0
#    NTDLLDIR0, WINE_HOST_PATH NT_HOST_PATH, and so on.
# The tools that build it keep their names (winebuild, winegcc, wrc,
# makedep's tools/wine in the build tree).

my $env_re = join('|', qw(SERVERSOCKET SERVER LOADERNOEXEC LOADER PREFIX DEBUG DLLOVERRIDES SYSTEMDLLPATH
                          DLLPATH DATADIR HOMEDIR CONFIGDIR DLLDIR USERNAME USERLOCALE UNIXCP ARCH
                          PRELOADRESERVE BUILDDIR));
my ($vars, $varfiles) = (0, 0);
find({ no_chdir => 1, wanted => sub {
    if (-d $_) { $File::Find::prune = 1 if m{/(\.git|autom4te\.cache)$}; return; }
    # C++ too: ICU (libs/icucommon/putil.cpp) finds icudtl.dat through
    # NTDATADIR; missed, PowerShell 7 (.NET) stopped at "Could not load ICU
    # data" after the update of 2026-11-16
    return unless /\.(c|h|cpp|cc|cxx|hpp|hh|m|in|inf|rc|rgs|reg|idl|l|y|ac|spec|po|sh|pl|man)$/;
    my $n = 0;
    $varfiles++ if edit($_, sub {
        $n += s/(?<![A-Za-z0-9_])WINE($env_re)(?![A-Za-z_])/NT$1/g;
        $n += s/(?<![A-Za-z0-9_])WINE_HOST_/NT_HOST_/g;
        $n += s/(?<![A-Za-z0-9_])WINE_(LAZY_FLUSH|REGISTRY_SAVE_PERIOD|IMAGE_CACHE)(?![A-Za-z_])/NT_$1/g;
    });
    $vars += $n;
}}, '.');
print "wine-names: environment: $vars names in $varfiles files\n";

# where the length of the old prefixes was counted
edit('dlls/ntdll/unix/env.c', sub {
    # a variable a Unix process gives a child, NT_HOST_X, is X in Windows
    s{(STARTS_WITH\( \*e, "NT_HOST_" \)\) continue;\s*if \(!strncmp\( \*e \+ )10,}{${1}8,} or die "env.c: no NT_HOST_ length\n";
    s{\{'W','I','N','E','_','H','O','S','T','_'\}}{{'N','T','_','H','O','S','T','_'}} or die "env.c: no host prefix array\n";
    # NTPATH, NTTEMP... set PATH, TEMP... of Windows, as WINEPATH did
    s{STARTS_WITH\( str, "WINE" \)\)(\s*\{\s*if \(is_special_env_var\( str \+ )4( \) \|\| is_ignored_env_var\( str \+ )4( \)\) str \+= )4;}
     {STARTS_WITH( str, "NT" ))${1}2${2}2${3}2;}s or die "env.c: no WINE prefix\n";
});

# the files, as ci/build-wine.sh installs them
edit('dlls/ntdll/unix/loader.c', sub {
    s{LIBDIR "/wine", BINDIR}{LIBDIR "/nt", BINDIR} or die "loader.c: no bin_dir\n";
    s{LIBDIR "/wine", DATADIR "/wine"}{LIBDIR "/nt", DATADIR "/nt"} or die "loader.c: no data_dir\n";
    s{wineloader = build_path\( ntdll_dir, "wine" \);}{wineloader = build_path( ntdll_dir, "ntloader" );} or die "loader.c: no loader\n";
    s{"%s%s/wine", dll_dir}{"%s%s/ntloader", dll_dir} or die "loader.c: no alternate loader\n";
    s{build_path_and_exec\( pid, bin_dir, "wineserver", argv \)}{build_path_and_exec( pid, bin_dir, "ntexec", argv )} or die "loader.c: no server\n";
    s{build_path_and_exec\( pid, path, "wineserver", argv \)}{build_path_and_exec( pid, path, "ntexec", argv )} or die "loader.c: no server in PATH\n";
    s{build_path_and_exec\( pid, BINDIR, "wineserver", argv \)}{build_path_and_exec( pid, BINDIR, "ntexec", argv )} or die "loader.c: no server in BINDIR\n";
    s{could not exec wineserver}{could not exec ntexec} or die "loader.c: no server message\n";
    # /usr/bin/notepad -> ntloader runs notepad.exe; ntloader itself does not look for ntloader.exe
    s{if \(strcmp\( basename, "wine" \)\)}{if (strcmp( basename, "ntloader" ) && strcmp( basename, "wine" ))} or die "loader.c: no loader name\n";
});
edit('tools/wine/wine.c', sub {
    s{"%s/wine%s/ntdll\.so"}{"%s/nt%s/ntdll.so"} or die "wine.c: no ntdll path\n";
});
edit('server/unicode.c', sub {
    s{DATADIR "/wine/nls"}{DATADIR "/nt/nls"}g == 2 or die "unicode.c: no nls dirs\n";
    s{"/usr/local/share/wine/nls", "/usr/share/wine/nls"}{"/usr/local/share/nt/nls", "/usr/share/nt/nls"} or die "unicode.c: no default nls dirs\n";
});
for my $file ('dlls/ntdll/unix/server.c', 'server/request.c') {
    edit($file, sub { s{"/tmp/\.wine-%u}{"/tmp/.nt-%u} or die "$file: no server directory\n" });
}
edit('dlls/ntdll/unix/server.c', sub {
    s{\? "wine" : "wineserver"}{? "ntloader" : "ntexec"} or die "server.c: no version message\n";
});
edit('programs/winboot/winboot.c', sub {
    s{\\\\wine\.inf}{\\\\nt.inf}g >= 3 or die "winboot.c: no setup file\n";
});
