#!/usr/bin/perl

use strict;
use warnings;
use ApMon;
my $apm = new ApMon(0);
select STDOUT; $| = 1;
select STDERR; $| = 1;
my $monalisahost = $ENV{MONALISAHOST};

if ((!defined $monalisahost) || ( $monalisahost eq "")) {
    $monalisahost = "lxbra0301.cern.ch";
}

my $apmonwarninglevel = $ENV{APMON_DEBUG_LEVEL};
my $apmonconfig = $ENV{APMON_CONFIG};

if ((!defined $apmonwarninglevel) || ($apmonwarninglevel eq "")) {
    $apmonwarninglevel = "WARNING";
}

if ((!defined $apmonconfig) || ($apmonconfig eq "")) {
    $apmonconfig = "['$monalisahost']";
}

$apm->setLogLevel($apmonwarninglevel);
$apm->setDestinations(["$monalisahost"]);

sub convert {
    my $val=shift;
    my $exp=shift;

    if ($exp eq "K") {$val *= 1000;}
    if ($exp eq "M") {$val *= 1000000;}
    if ($exp eq "G") {$val *= 1000000000;}
    if ($exp eq "T") {$val *= 1000000000000;}
    return $val;
}


while (1) {
    open NS, "/opt/eos/bin/eos root://localhost ns stat -a |";
    my $infohash;

    while (<NS>) {
	my @tags = split " ",$_;

	if ($_ =~ /^#/) {
	    $infohash = {};
	    next;
	}

	if ($_ =~ /Files/) {
	    #nofiles
	    $infohash->{id} = "ns";
	    $infohash->{nfiles} = $tags[2];
	    next;
	}

	if ($_ =~ /Directories/) {
	    $infohash->{id} = "ns";
	    $infohash->{ndirs} = $tags[2];
	    $apm->sendParameters('Size', 'Total',
				 'nfiles',$infohash->{nfiles}, 
				 'ndirs', $infohash->{ndirs});
	    $infohash = {};
	    next;
	}
			     
	if ($_ =~ /^ALL/) {
	    #sums
	    $infohash->{id} = "all";
	    $infohash->{cmd} = $tags[1];
	    $infohash->{sum} = $tags[2];
	    $infohash->{'5s'} = $tags[3];
	    $infohash->{'1m'} = $tags[4];
	    $infohash->{'5m'} = $tags[5];
	    $infohash->{'1h'} = $tags[6];
	}

	if ($_ =~ /^uid=/) {
	    #uid values
	    my $uid = $tags[0];
	    $uid =~ s/uid=/uid::/;
	    $infohash->{id} = $uid; 
	    $infohash->{cmd} = $tags[1];
	    $infohash->{sum} = $tags[2];
	    $infohash->{'5s'} = $tags[3];
	    $infohash->{'1m'} = $tags[4];
	    $infohash->{'5m'} = $tags[5];
	    $infohash->{'1h'} = $tags[6];
	}

	if ($_ =~ /^gid=/) {
	    #gid values
	    my $gid = $tags[0];
	    $gid =~ s/gid=/gid::/;
	    $infohash->{id} = $gid; 
	    $infohash->{cmd} = $tags[1];
	    $infohash->{sum} = $tags[2];
	    $infohash->{'5s'} = $tags[3];
	    $infohash->{'1m'} = $tags[4];
	    $infohash->{'5m'} = $tags[5];
	    $infohash->{'1h'} = $tags[6];
	}

	if (defined $infohash->{id}) {
#	    printf("Sending %s %s %s %s %s %s %s \n", $infohash->{id}, $infohash->{cmd},$infohash->{sum}, $infohash->{'5s'}, $infohash->{'1m'}, $infohash->{'5m'}, $infohash->{'1h'});
	    my $shortid=$infohash->{id};
	    $shortid .= "::";
	    $shortid .= $infohash->{cmd};

	    $apm->sendParameters('Rates', $shortid,
				 'id',$infohash->{id}, 
				 'cmd',$infohash->{cmd},
				 'sum',$infohash->{sum},
				 '5s',$infohash->{'5s'},
				 '1m',$infohash->{'1m'},
				 '5m',$infohash->{'5m'},
				 '1h',$infohash->{'1h'},
				 );
	    $infohash = {};
	}
    }

    close NS;

    open NS, "/opt/eos/bin/eos root://localhost fs ls |";
    while (<NS>) {
	my @tags = split " ",$_;

	if ( ($_ =~ /online/) || ($_ =~ /offline/) ) {
	    $infohash = {};
	    $infohash->{id} = $tags[0];
	    if ($_ =~ /online/) {
		$infohash->{online} = 1;
		$infohash->{offline} = 0;
	    } else {
		$infohash->{online} = 0;
		$infohash->{offline} = 1;
	    }
	    $apm->sendParameters('Fs', $infohash->{id},
				 'online',$infohash->{online}, 
				 'offline',$infohash->{offline});
	} else {
	    if (defined $tags[0] && $tags[0] =~ /^\/eos/) {
		my $blocks = convert($tags[7],$tags[8]);
		my $free   = convert($tags[9],$tags[10]);
		my $files  = convert($tags[11],$tags[12]);

		$apm->sendParameters('FsFileSystems', $tags[1],
				     'id', $tags[1],
				     'path', $tags[2],
				     'schedgroup', $tags[3],
				     'bootstatus', $tags[4],
				     'bt', $tags[5],
				     'configstatus', $tags[6],
				     'blocks', $blocks,
				     'free', $free,
				     'files', $files,
				     'ropen', $tags[13],
				     'wopen', $tags[14],
				     'ec', $tags[15],
				     'emsg', $tags[16]);
		
	    }
	}

	
    }

    close NS;

    open NS, "/opt/eos/bin/eos root://localhost quota ls |";

    my $id="";
    while (<NS>) {


	my @tags = split " ",$_;
	if ($_ =~ /^USER  SPACE/) {
	    $id = "uid";
	} 
	
	if ($_ =~ /^GROUP SPACE/) {
	    $id = "gid";
	}

	if (!defined $tags[0]) {
	    next;
	}

	if (defined $tags[0] && $tags[0] eq "PHYS") {
	    my $usedbytes = convert($tags[2],$tags[3]);
	    my $usedfiles = convert($tags[4],$tags[5]);
	    my $freebytes = convert($tags[6],$tags[7]);
	    my $freefiles = convert($tags[8],$tags[9]);

	    # send info per space
	    $shortid .= $tags[1];
	    $apm->sendParameters('Quota_Physical', $shortid,
				 'usedbytes',$usedbytes,
				 'usedfiles',$usedfiles,
				 'freebytes',$freebytes,
				 'freefiles',$freefiles,
				 'volume_usage',$tags[10],
				 'inodes_usage',$tags[12]
				 );
	    next;
	}

	if (defined $tags[0] && $tags[0] eq "ALL") {
	    # summary
	    my $usedbytes = convert($tags[2],$tags[3]);
	    my $usedfiles = convert($tags[4],$tags[5]);
	    my $freebytes = convert($tags[6],$tags[7]);
	    my $freefiles = convert($tags[8],$tags[9]);
	    $apm->sendParameters('Quota_Virtual', $tags[1],
				 'usedbytes',$usedbytes,
				 'usedfiles',$usedfiles,
				 'freebytes',$freebytes,
				 'freefiles',$freefiles,
				 'volume_usage',$tags[10],
				 );
	    next;
	}

	if (defined $tags[10] && defined $tags[1] && $tags[1] ne "SPACE") {
	    # user info
	    my $shortid = $id;
	    $shortid .= "::";
	    $shortid .= $tags[0];
	    $shortid .= "::";
	    $shortid .= $tags[1];
	    my $usedbytes = convert($tags[2],$tags[3]);
	    my $usedfiles = convert($tags[4],$tags[5]);
	    my $freebytes = convert($tags[6],$tags[7]);
	    my $freefiles = convert($tags[8],$tags[9]);
            $apm->sendParameters('Quota', $shortid,
                                 'usedbytes',$usedbytes,
                                 'usedfiles',$usedfiles,
                                 'freebytes',$freebytes,
                                 'freefiles',$freefiles,
                                 'volume_usage',$tags[10],
                                 );
	}
    }
    
    close NS;

    sleep(10);
}
