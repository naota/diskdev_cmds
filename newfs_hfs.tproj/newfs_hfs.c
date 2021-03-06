/*
 * Copyright (c) 1999-2008 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */


#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <paths.h>
#include <pwd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <syslog.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>

#include <IOKit/storage/IOMediaBSDClient.h>

#include <hfs/hfs_format.h>
#include "newfs_hfs.h"

#if __STDC__
#include <stdarg.h>
#else
#include <varargs.h>
#endif

#define	NOVAL       (-1)
#define UMASK       (0755)
#define	ACCESSMASK  (0777)

#define ROUNDUP(x,y) (((x)+(y)-1)/(y)*(y))

static void getnodeopts __P((char* optlist));
static void getclumpopts __P((char* optlist));
static gid_t a_gid __P((char *));
static uid_t a_uid __P((char *));
static mode_t a_mask __P((char *));
static int hfs_newfs __P((char *device));
static void validate_hfsplus_block_size __P((UInt64 sectorCount, UInt32 sectorSize));
static void hfsplus_params __P((const DriveInfo* dip, hfsparams_t *defaults));
static UInt32 clumpsizecalc __P((UInt32 clumpblocks));
static UInt32 CalcHFSPlusBTreeClumpSize __P((UInt32 blockSize, UInt32 nodeSize, UInt64 sectors, int fileID));
static void usage __P((void));


char	*progname;
char	gVolumeName[kHFSPlusMaxFileNameChars + 1] = {kDefaultVolumeNameStr};
char	rawdevice[MAXPATHLEN];
char	blkdevice[MAXPATHLEN];
uint32_t gBlockSize = 0;
UInt32	gNextCNID = kHFSFirstUserCatalogNodeID;

time_t  createtime;

int	gNoCreate = FALSE;
int	gUserCatNodeSize = FALSE;
int	gCaseSensitive = FALSE;
int	gUserAttrSize = FALSE;

#define JOURNAL_DEFAULT_SIZE (8*1024*1024)
int     gJournaled = FALSE;
char    *gJournalDevice = NULL;
UInt64	gJournalSize = 0;

uid_t	gUserID = (uid_t)NOVAL;
gid_t	gGroupID = (gid_t)NOVAL;
mode_t	gModeMask = (mode_t)NOVAL;

UInt64	gPartitionSize = 0;

UInt32	catnodesiz = 8192;
UInt32	extnodesiz = 4096;
UInt32	atrnodesiz = 8192;

UInt32	catclumpblks = 0;
UInt32	extclumpblks = 0;
UInt32	atrclumpblks = 0;
UInt32	bmclumpblks = 0;
UInt32	rsrclumpblks = 0;
UInt32	datclumpblks = 0;
uint32_t hfsgrowblks = 0;      /* maximum growable size of wrapper */


UInt64
get_num(char *str)
{
    UInt64 num;
    char *ptr;

    num = strtoull(str, &ptr, 0);

    if (*ptr) {
	    char scale = tolower(*ptr);

	    switch(scale) {
	    case 'b':
		    num *= 512ULL;
		    break;
	    case 'p':
		    num *= 1024ULL;
		    /* fall through */
	    case 't':
		    num *= 1024ULL;
		    /* fall through */
	    case 'g':
		    num *= 1024ULL;
		    /* fall through */
	    case 'm':
		    num *= 1024ULL;
		    /* fall through */
	    case 'k':
		    num *= 1024ULL;
		    break;

	    default:
		    num = 0ULL;
		    break;
	}
    }
    return num;
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	extern char *optarg;
	extern int optind;
	int ch;
	char *cp, *special;
	struct statfs *mp;
	int n;
	
	if ((progname = strrchr(*argv, '/')))
		++progname;
	else
		progname = *argv;


	while ((ch = getopt(argc, argv, "G:J:D:M:N:U:hsb:c:i:n:v:")) != EOF)
		switch (ch) {
		case 'G':
			gGroupID = a_gid(optarg);
			break;

		case 'J':
			gJournaled = TRUE;
			if (isdigit(optarg[0])) {
			    gJournalSize = get_num(optarg);
			    if (gJournalSize < 512*1024) {
					printf("%s: journal size %lldk too small.  Reset to %dk.\n",
						progname, gJournalSize/1024, JOURNAL_DEFAULT_SIZE/1024);
					gJournalSize = JOURNAL_DEFAULT_SIZE;
			    }
			} else {
				/* back up because there was no size argument */
			    optind--;
			}
			break;
			
		case 'D':
			gJournalDevice = (char *)optarg;
			break;
			
		case 'N':
			gNoCreate = TRUE;
			if (isdigit(optarg[0])) {
				gPartitionSize = get_num(optarg);
			} else {
				/* back up because there was no size argument */
				optind--;
			}
			break;

		case 'M':
			gModeMask = a_mask(optarg);
			break;

		case 'U':
			gUserID = a_uid(optarg);
			break;

		case 'b':
		{
			UInt64 tempBlockSize;
			
			tempBlockSize = get_num(optarg);
			if (tempBlockSize < HFSMINBSIZE)
				fatal("%s: bad allocation block size (too small)", optarg);
			if (tempBlockSize > HFSMAXBSIZE) 
				fatal("%s: bad allocation block size (too large)", optarg);
			gBlockSize = tempBlockSize;
			break;
		}

		case 'c':
			getclumpopts(optarg);
			break;

		case 'i':
			gNextCNID = atoi(optarg);
			/*
			 * make sure its at least kHFSFirstUserCatalogNodeID
			 */
			if (gNextCNID < kHFSFirstUserCatalogNodeID)
				fatal("%s: starting catalog node id too small (must be > 15)", optarg);
			break;

		case 'n':
			getnodeopts(optarg);
			break;

		case 's':
			gCaseSensitive = TRUE;
			break;

		case 'v':
			n = strlen(optarg);
			if (n > (sizeof(gVolumeName) - 1))
				fatal("\"%s\" is too long (%d byte maximum)",
				      optarg, sizeof(gVolumeName) - 1);
			if (n == 0)
				fatal("name required with -v option");
			strlcpy(gVolumeName, optarg, sizeof(gVolumeName));
			break;

		case '?':
		default:
			usage();
		}

	argc -= optind;
	argv += optind;

	if (gPartitionSize != 0) {
		/*
		 * If we are given -N, a size, and a device, that's a usage error.
		 */
		if (argc != 0)
			usage();

		rawdevice[0] = blkdevice[0] = 0;
	} else {
		if (argc != 1)
			usage();

		special = argv[0];
		cp = strrchr(special, '/');
		if (cp != 0)
			special = cp + 1;
		if (*special == 'r')
			special++;
		(void) snprintf(rawdevice, sizeof(rawdevice), "%sr%s", _PATH_DEV, special);
		(void) snprintf(blkdevice, sizeof(blkdevice), "%s%s", _PATH_DEV, special);
	}

	if (gPartitionSize == 0) {
		/*
		 * Check if target device is aready mounted
		 */
		n = getmntinfo(&mp, MNT_NOWAIT);
		if (n == 0)
			fatal("%s: getmntinfo: %s", blkdevice, strerror(errno));

		while (--n >= 0) {
			if (strcmp(blkdevice, mp->f_mntfromname) == 0)
				fatal("%s is mounted on %s", blkdevice, mp->f_mntonname);
			++mp;
		}
	}

	if (hfs_newfs(rawdevice) < 0) {
		err(1, "cannot create filesystem on %s", rawdevice);
	}

	exit(0);
}


static void getnodeopts(char* optlist)
{
	char *strp = optlist;
	char *ndarg;
	char *p;
	UInt32 ndsize;
	
	while((ndarg = strsep(&strp, ",")) != NULL && *ndarg != '\0') {

		p = strchr(ndarg, '=');
		if (p == NULL)
			usage();
	
		ndsize = atoi(p+1);

		switch (*ndarg) {
		case 'c':
			if (ndsize < 4096 || ndsize > 32768 || (ndsize & (ndsize-1)) != 0)
				fatal("%s: invalid catalog b-tree node size", ndarg);
			catnodesiz = ndsize;
			gUserCatNodeSize = TRUE;
			break;

		case 'e':
			if (ndsize < 1024 || ndsize > 32768 || (ndsize & (ndsize-1)) != 0)
				fatal("%s: invalid extents b-tree node size", ndarg);
			extnodesiz = ndsize;
			break;

		case 'a':
			if (ndsize < 1024 || ndsize > 32768 || (ndsize & (ndsize-1)) != 0)
				fatal("%s: invalid atrribute b-tree node size", ndarg);
			atrnodesiz = ndsize;
			break;

		default:
			usage();
		}
	}
}


static void getclumpopts(char* optlist)
{
	char *strp = optlist;
	char *ndarg;
	char *p;
	UInt32 clpblocks;
	
	while((ndarg = strsep(&strp, ",")) != NULL && *ndarg != '\0') {

		p = strchr(ndarg, '=');
		if (p == NULL)
			usage();
			
		clpblocks = atoi(p+1);
		
		switch (*ndarg) {
		case 'a':
			atrclumpblks = clpblocks;
			gUserAttrSize = TRUE;
			break;
		case 'b':
			bmclumpblks = clpblocks;
			break;
		case 'c':
			catclumpblks = clpblocks;
			break;
		case 'd':
			datclumpblks = clpblocks;
			break;
		case 'e':
			extclumpblks = clpblocks;
			break;
		case 'r':
			rsrclumpblks = clpblocks;
			break;

		default:
			usage();
		}
	}
}

gid_t
static a_gid(char *s)
{
	struct group *gr;
	char *gname;
	gid_t gid = 0;

	if ((gr = getgrnam(s)) != NULL)
		gid = gr->gr_gid;
	else {
		for (gname = s; *s && isdigit(*s); ++s);
		if (!*s)
			gid = atoi(gname);
		else
			errx(1, "unknown group id: %s", gname);
	}
	return (gid);
}

static uid_t
a_uid(char *s)
{
	struct passwd *pw;
	char *uname;
	uid_t uid = 0;

	if ((pw = getpwnam(s)) != NULL)
		uid = pw->pw_uid;
	else {
		for (uname = s; *s && isdigit(*s); ++s);
		if (!*s)
			uid = atoi(uname);
		else
			errx(1, "unknown user id: %s", uname);
	}
	return (uid);
}

static mode_t
a_mask(char *s)
{
	int done, rv;
	char *ep;

	done = 0;
	rv = -1;
	if (*s >= '0' && *s <= '7') {
		done = 1;
		rv = strtol(s, &ep, 8);
	}
	if (!done || rv < 0 || *ep)
		errx(1, "invalid access mask: %s", s);
	return (rv);
}


/*
 * Validate the HFS Plus allocation block size in gBlockSize.  If none was
 * specified, then calculate a suitable default.
 *
 * Modifies the global variable gBlockSize.
 */
static void validate_hfsplus_block_size(UInt64 sectorCount, UInt32 sectorSize)
{
	if (gBlockSize == 0) {
		/* Compute a default allocation block size based on volume size */
		gBlockSize = DFL_BLKSIZE;	/* Prefer the default of 4K */
		
		/* Use a larger power of two if total blocks would overflow 32 bits */
		while ((sectorCount / (gBlockSize / sectorSize)) > 0xFFFFFFFF) {
			gBlockSize <<= 1;	/* Must be a power of two */
		}
	} else {
		/* Make sure a user-specified block size is reasonable */
		if ((gBlockSize & (gBlockSize-1)) != 0)
			fatal("%s: bad HFS Plus allocation block size (must be a power of two)", optarg);
	
		if ((sectorCount / (gBlockSize / sectorSize)) > 0xFFFFFFFF)
			fatal("%s: block size is too small for %lld sectors", optarg, gBlockSize, sectorCount);

		if (gBlockSize < HFSOPTIMALBLKSIZE)
			warnx("Warning: %ld is a non-optimal block size (4096 would be a better choice)", gBlockSize);
	}
}



static int
hfs_newfs(char *device)
{
	struct stat stbuf;
	DriveInfo dip = { 0 };
	int fso = -1;
	int retval = 0;
	hfsparams_t defaults = {0};
	UInt64 maxPhysPerIO = 0;

	if (gPartitionSize) {
		dip.sectorSize = kBytesPerSector;
		dip.physTotalSectors = dip.totalSectors = gPartitionSize / kBytesPerSector;
		dip.physSectorSize = kBytesPerSector;	/* 512-byte sectors */
		dip.fd = 0;
	} else {
		if (gNoCreate) {
			fso = open( device, O_RDONLY | O_NDELAY, 0 );
		} else {
			fso = open( device, O_RDWR | O_NDELAY, 0 );
		}
		if (fso == -1) {
			return -1;
		}

		dip.fd = fso;
		fcntl(fso, F_NOCACHE, 1);

		if (fso < 0)
			fatal("%s: %s", device, strerror(errno));

		if (fstat( fso, &stbuf) < 0)
			fatal("%s: %s", device, strerror(errno));

		if (ioctl(fso, DKIOCGETBLOCKSIZE, &dip.physSectorSize) < 0)
			fatal("%s: %s", device, strerror(errno));

		if ((dip.physSectorSize % kBytesPerSector) != 0)
			fatal("%d is an unsupported sector size\n", dip.physSectorSize);

		if (ioctl(fso, DKIOCGETBLOCKCOUNT, &dip.physTotalSectors) < 0)
			fatal("%s: %s", device, strerror(errno));

	}

	dip.physSectorsPerIO = (1024 * 1024) / dip.physSectorSize;  /* use 1M as default */

	if (fso != -1 && ioctl(fso, DKIOCGETMAXBLOCKCOUNTREAD, &maxPhysPerIO) < 0)
		fatal("%s: %s", device, strerror(errno));

	if (maxPhysPerIO)
		dip.physSectorsPerIO = MIN(dip.physSectorsPerIO, maxPhysPerIO);

	if (fso != -1 && ioctl(fso, DKIOCGETMAXBLOCKCOUNTWRITE, &maxPhysPerIO) < 0)
		fatal("%s: %s", device, strerror(errno));

	if (maxPhysPerIO)
		dip.physSectorsPerIO = MIN(dip.physSectorsPerIO, maxPhysPerIO);

	if (fso != -1 && ioctl(fso, DKIOCGETMAXBYTECOUNTREAD, &maxPhysPerIO) < 0)
		fatal("%s: %s", device, strerror(errno));

	if (maxPhysPerIO)
		dip.physSectorsPerIO = MIN(dip.physSectorsPerIO, maxPhysPerIO / dip.physSectorSize);

	if (fso != -1 && ioctl(fso, DKIOCGETMAXBYTECOUNTWRITE, &maxPhysPerIO) < 0)
		fatal("%s: %s", device, strerror(errno));

	if (maxPhysPerIO)
		dip.physSectorsPerIO = MIN(dip.physSectorsPerIO, maxPhysPerIO / dip.physSectorSize);

	dip.sectorSize = kBytesPerSector;
	dip.totalSectors = dip.physTotalSectors * dip.physSectorSize / dip.sectorSize;

	dip.sectorOffset = 0;
	time(&createtime);

	/*
	 * If we're going to make an HFS Plus disk (with or without a wrapper), validate the
	 * HFS Plus allocation block size.  This will also calculate a default allocation
	 * block size if none (or zero) was specified.
	 */
	validate_hfsplus_block_size(dip.totalSectors, dip.sectorSize);	

	/* Make an HFS Plus disk */	

	if ((dip.totalSectors * dip.sectorSize ) < kMinHFSPlusVolumeSize)
		fatal("%s: partition is too small (minimum is %d KB)", device, kMinHFSPlusVolumeSize/1024);

	hfsplus_params(&dip, &defaults);
	if (gNoCreate == 0) {
		retval = make_hfsplus(&dip, &defaults);
		if (retval == 0) {
			printf("Initialized %s as a ", device);
			if (dip.totalSectors > 2048ULL*1024*1024)
				printf("%ld TB",
						(long)((dip.totalSectors + (1024ULL*1024*1024))/(2048ULL*1024*1024)));
			else if (dip.totalSectors > 2048*1024)
				printf("%ld GB",
						(long)((dip.totalSectors + (1024*1024))/(2048*1024)));
			else if (dip.totalSectors > 2048)
				printf("%ld MB",
						(long)((dip.totalSectors + 1024)/2048));
			else
				printf("%ld KB",
						(long)((dip.totalSectors + 1)/2));
			if (gJournaled)
				printf(" HFS Plus volume with a %uk journal\n",
						(int)defaults.journalSize/1024);
			else
				printf(" HFS Plus volume\n");
		}
	}

	if (retval)
		fatal("%s: %s", device, strerror(errno));

	if ( fso > 0 ) {
		close(fso);
	}

	return retval;
}


static void hfsplus_params (const DriveInfo* dip, hfsparams_t *defaults)
{
	UInt64  sectorCount = dip->totalSectors;
	UInt32  sectorSize = dip->sectorSize;
	uint32_t totalBlocks;
	UInt32	minClumpSize;
	UInt32	clumpSize;
	UInt32	oddBitmapBytes;
	
	defaults->flags = 0;
	defaults->blockSize = gBlockSize;
	defaults->nextFreeFileID = gNextCNID;
	defaults->createDate = createtime + MAC_GMT_FACTOR;     /* Mac OS GMT time */
	defaults->hfsAlignment = 0;
	defaults->journaledHFS = gJournaled;
	defaults->journalDevice = gJournalDevice;

	/*
	 * If any of the owner, group or mask are set then
	 * make sure they're all valid and pass them down.
	 */
	if (gUserID != (uid_t)NOVAL  ||
	    gGroupID != (gid_t)NOVAL ||
	    gModeMask != (mode_t)NOVAL) {
		defaults->owner = (gUserID == (uid_t)NOVAL) ? geteuid() : gUserID;
		defaults->group = (gGroupID == (gid_t)NOVAL) ? getegid() : gGroupID;
		defaults->mask = (gModeMask == (mode_t)NOVAL) ? UMASK : (gModeMask & ACCESSMASK);
		
		defaults->flags |= kUseAccessPerms;
	}

	/*
	 * We want at least 8 megs of journal for each 100 gigs of
	 * disk space.  We cap the size at 512 megs (64x default), unless
	 * the allocation block size is larger, in which case we use one
	 * allocation block.
	 *
	 * Only scale if it's the default, otherwise just take what
	 * the user specified.
	 */
	if (gJournaled) {
	    if (gJournalSize == 0) {
		UInt32 jscale;
		
		jscale = (sectorCount * sectorSize) / ((UInt64)100 * 1024 * 1024 * 1024);
		if (jscale > 64)
		    jscale = 64;
		defaults->journalSize = JOURNAL_DEFAULT_SIZE * (jscale + 1);
	    } else {
		defaults->journalSize = gJournalSize;
	    }
	    if (defaults->journalSize > 512 * 1024 * 1024) {
		defaults->journalSize = 512 * 1024 * 1024;
	    }
	    if (defaults->journalSize < defaults->blockSize) {
	    	defaults->journalSize = defaults->blockSize;
	    }
	}
	
	// volumes that are 128 megs or less in size have such
	// a small bitmap (one 4k-block) and inherhently such
	// a small btree that we can get by with a much smaller
	// journal.  even in a worst case scenario of a catalog
	// filled with very long korean file names we should
	// never touch more than 256k of meta-data for a single
	// transaction.  therefore we'll make the journal 512k
	// which is safe and doesn't waste much space.
	//
	if (sectorCount * sectorSize < 128*1024*1024) {
		defaults->journalSize = 512 * 1024;
	}

	strncpy((char *)defaults->volumeName, gVolumeName, sizeof(defaults->volumeName) - 1);
	defaults->volumeName[sizeof(defaults->volumeName) - 1] = '\0';

	if (rsrclumpblks == 0) {
		if (gBlockSize > DFL_BLKSIZE)
			defaults->rsrcClumpSize = ROUNDUP(kHFSPlusRsrcClumpFactor * DFL_BLKSIZE, gBlockSize);
		else
			defaults->rsrcClumpSize = kHFSPlusRsrcClumpFactor * gBlockSize;
	} else
		defaults->rsrcClumpSize = clumpsizecalc(rsrclumpblks);

	if (datclumpblks == 0) {
		if (gBlockSize > DFL_BLKSIZE)
			defaults->dataClumpSize = ROUNDUP(kHFSPlusRsrcClumpFactor * DFL_BLKSIZE, gBlockSize);
		else
			defaults->dataClumpSize = kHFSPlusRsrcClumpFactor * gBlockSize;
	} else
		defaults->dataClumpSize = clumpsizecalc(datclumpblks);

	/*
	 * The default  b-tree node size is 8K.  However, if the
	 * volume is small (< 1 GB) we use 4K instead.
	 */
	if (!gUserCatNodeSize) {
		if ((gBlockSize < HFSOPTIMALBLKSIZE) ||
		    ((UInt64)(sectorCount * sectorSize) < (UInt64)0x40000000))
			catnodesiz = 4096;
	}

	if (catclumpblks == 0) {
		clumpSize = CalcHFSPlusBTreeClumpSize(gBlockSize, catnodesiz, sectorCount, kHFSCatalogFileID);
	}
	else {
		clumpSize = clumpsizecalc(catclumpblks);
		
		if (clumpSize % catnodesiz != 0)
			fatal("c=%ld: clump size is not a multiple of node size\n", clumpSize/gBlockSize);
	}
	defaults->catalogClumpSize = clumpSize;
	defaults->catalogNodeSize = catnodesiz;
	if (gBlockSize < 4096 && gBlockSize < catnodesiz)
		warnx("Warning: block size %ld is less than catalog b-tree node size %ld", gBlockSize, catnodesiz);

	if (extclumpblks == 0) {
		clumpSize = CalcHFSPlusBTreeClumpSize(gBlockSize, extnodesiz, sectorCount, kHFSExtentsFileID);
	}
	else {
		clumpSize = clumpsizecalc(extclumpblks);
		if (clumpSize % extnodesiz != 0)
			fatal("e=%ld: clump size is not a multiple of node size\n", clumpSize/gBlockSize);
	}
	defaults->extentsClumpSize = clumpSize;
	defaults->extentsNodeSize = extnodesiz;
	if (gBlockSize < extnodesiz)
		warnx("Warning: block size %ld is less than extents b-tree node size %ld", gBlockSize, extnodesiz);

	if (atrclumpblks == 0) {
		if (gUserAttrSize) {
			clumpSize = 0;
		}
		else {
			clumpSize = CalcHFSPlusBTreeClumpSize(gBlockSize, atrnodesiz, sectorCount, kHFSAttributesFileID);
		}
	}
	else {
		clumpSize = clumpsizecalc(atrclumpblks);
		if (clumpSize % atrnodesiz != 0)
			fatal("a=%ld: clump size is not a multiple of node size\n", clumpSize/gBlockSize);
	}
	defaults->attributesClumpSize = clumpSize;
	defaults->attributesNodeSize = atrnodesiz;

	/*
	 * Calculate the number of blocks needed for bitmap (rounded up to a multiple of the block size).
	 */
	
	/*
	 * Figure out how many bytes we need for the given totalBlocks
	 * Note: this minimum value may be too large when it counts the
	 * space used by the wrapper
	 */
	totalBlocks = sectorCount / (gBlockSize / sectorSize);

	minClumpSize = totalBlocks >> 3;	/* convert bits to bytes by dividing by 8 */
	if (totalBlocks & 7)
		++minClumpSize;	/* round up to whole bytes */
	
	/* Round up to a multiple of blockSize */
	if ((oddBitmapBytes = minClumpSize % gBlockSize))
		minClumpSize = minClumpSize - oddBitmapBytes + gBlockSize;

	if (bmclumpblks == 0) {
		clumpSize = minClumpSize;
	}
	else {
		clumpSize = clumpsizecalc(bmclumpblks);

		if (clumpSize < minClumpSize)
			fatal("b=%ld: bitmap clump size is too small\n", clumpSize/gBlockSize);
	}
	defaults->allocationClumpSize = clumpSize;
	
	if (gCaseSensitive)
		defaults->flags |= kMakeCaseSensitive;

	if (gNoCreate) {
		if (gPartitionSize == 0)
			printf("%llu sectors (%u bytes per sector)\n", dip->physTotalSectors, dip->physSectorSize);
		printf("HFS Plus format parameters:\n");
		printf("\tvolume name: \"%s\"\n", gVolumeName);
		printf("\tblock-size: %u\n", defaults->blockSize);
		printf("\ttotal blocks: %u\n", totalBlocks);
		if (gJournaled)
			printf("\tjournal-size: %uk\n", defaults->journalSize/1024);
		printf("\tfirst free catalog node id: %u\n", defaults->nextFreeFileID);
		printf("\tcatalog b-tree node size: %u\n", defaults->catalogNodeSize);
		printf("\tinitial catalog file size: %u\n", defaults->catalogClumpSize);
		printf("\textents b-tree node size: %u\n", defaults->extentsNodeSize);
		printf("\tinitial extents file size: %u\n", defaults->extentsClumpSize);
		printf("\tattributes b-tree node size: %u\n", defaults->attributesNodeSize);
		printf("\tinitial attributes file size: %u\n", defaults->attributesClumpSize);
		printf("\tinitial allocation file size: %u (%u blocks)\n",
			defaults->allocationClumpSize, defaults->allocationClumpSize / gBlockSize);
		printf("\tdata fork clump size: %u\n", defaults->dataClumpSize);
		printf("\tresource fork clump size: %u\n", defaults->rsrcClumpSize);
		if (defaults->flags & kUseAccessPerms) {
			printf("\tuser ID: %d\n", (int)defaults->owner);
			printf("\tgroup ID: %d\n", (int)defaults->group);
			printf("\taccess mask: %o\n", (int)defaults->mask);
		}
	}
}


static UInt32
clumpsizecalc(UInt32 clumpblocks)
{
	UInt64 clumpsize;

	clumpsize = (UInt64)clumpblocks * (UInt64)gBlockSize;
		
	if (clumpsize & (UInt64)(0xFFFFFFFF00000000ULL))
		fatal("=%ld: too many blocks for clump size!", clumpblocks);

	return ((UInt32)clumpsize);
}


#define CLUMP_ENTRIES	15

short clumptbl[CLUMP_ENTRIES * 3] = {
/*
 *	    Volume	Attributes	 Catalog	 Extents
 *	     Size	Clump (MB)	Clump (MB)	Clump (MB)
 */
	/*   1GB */	  4,		  4,		 4,
	/*   2GB */	  6,		  6,		 4,
	/*   4GB */	  8,		  8,		 4,
	/*   8GB */	 11,		 11,		 5,
	/*
	 * For volumes 16GB and larger, we want to make sure that a full OS
	 * install won't require fragmentation of the Catalog or Attributes
	 * B-trees.  We do this by making the clump sizes sufficiently large,
	 * and by leaving a gap after the B-trees for them to grow into.
	 *
	 * For SnowLeopard 10A298, a FullNetInstall with all packages selected
	 * results in:
	 * Catalog B-tree Header
	 *	nodeSize:          8192
	 *	totalNodes:       31616
	 *	freeNodes:         1978
	 * (used = 231.55 MB)
	 * Attributes B-tree Header
	 *	nodeSize:          8192
	 *	totalNodes:       63232
	 *	freeNodes:          958
	 * (used = 486.52 MB)
	 *
	 * We also want Time Machine backup volumes to have a sufficiently
	 * large clump size to reduce fragmentation.
	 *
	 * The series of numbers for Catalog and Attribute form a geometric series.
	 * For Catalog (16GB to 512GB), each term is 8**(1/5) times the previous
	 * term.  For Attributes (16GB to 512GB), each term is 4**(1/5) times
	 * the previous term.  For 1TB to 16TB, each term is 2**(1/5) times the
	 * previous term.
	 */
	/*  16GB */	 64,		 32,		 5,
	/*  32GB */	 84,		 49,		 6,
	/*  64GB */	111,		 74,		 7,
	/* 128GB */	147,		111,		 8,
	/* 256GB */	194,		169,		 9,
	/* 512GB */	256,		256,		11,
	/*   1TB */	294,		294,		14,
	/*   2TB */	338,		338,		16,
	/*   4TB */	388,		388,		20,
	/*   8TB */	446,		446,		25,
	/*  16TB */	512,		512,		32
};

/*
 * CalcHFSPlusBTreeClumpSize
 *	
 * This routine calculates the file clump size for either
 * the catalog file or the extents overflow file.
 */
static UInt32
CalcHFSPlusBTreeClumpSize(UInt32 blockSize, UInt32 nodeSize, UInt64 sectors, int fileID)
{
	UInt32 mod = MAX(nodeSize, blockSize);
	UInt32 clumpSize;
	int column;
	int i;

	/* Figure out which column of the above table to use for this file. */
	switch (fileID) {
		case kHFSAttributesFileID:
			column = 0;
			break;
		case kHFSCatalogFileID:
			column = 1;
			break;
		default:
			column = 2;
			break;
	}
	
	/*
	 * The default clump size is 0.8% of the volume size. And
	 * it must also be a multiple of the node and block size.
	 */
	if (sectors < 0x200000) {
		clumpSize = sectors << 2;	/*  0.8 %  */
		if (clumpSize < (8 * nodeSize))
			clumpSize = 8 * nodeSize;
	} else {
		/*
		 * XXX This should scale more smoothly!
		 */
		/* turn exponent into table index... */
		for (i = 0, sectors = sectors >> 22;
		     sectors && (i < CLUMP_ENTRIES-1);
		     ++i, sectors = sectors >> 1);
		
		clumpSize = clumptbl[column + (i) * 3] * 1024 * 1024;
	}
	
	/*
	 * Round the clump size to a multiple of node and block size.
	 * NOTE: This rounds down.
	 */
	clumpSize /= mod;
	clumpSize *= mod;
	
	/*
	 * Rounding down could have rounded down to 0 if the block size was
	 * greater than the clump size.  If so, just use one block or node.
	 */
	if (clumpSize == 0)
		clumpSize = mod;
		
	return (clumpSize);
}


/* VARARGS */
void
#if __STDC__
fatal(const char *fmt, ...)
#else
fatal(fmt, va_alist)
	char *fmt;
	va_dcl
#endif
{
	va_list ap;

#if __STDC__
	va_start(ap, fmt);
#else
	va_start(ap);
#endif
	if (fcntl(STDERR_FILENO, F_GETFL) < 0) {
		openlog(progname, LOG_CONS, LOG_DAEMON);
		vsyslog(LOG_ERR, fmt, ap);
		closelog();
	} else {
		vwarnx(fmt, ap);
	}
	va_end(ap);
	exit(1);
	/* NOTREACHED */
}


void usage()
{
	fprintf(stderr, "usage: %s [-N [partition-size]] [hfsplus-options] special-device\n", progname);

	fprintf(stderr, "  options:\n");
	fprintf(stderr, "\t-N do not create file system, just print out parameters\n");
	fprintf(stderr, "\t-s use case-sensitive filenames (default is case-insensitive)\n");

	fprintf(stderr, "  where hfsplus-options are:\n");
	fprintf(stderr, "\t-J [journal-size] make this HFS+ volume journaled\n");
	fprintf(stderr, "\t-D journal-dev use 'journal-dev' for an external journal\n");
	fprintf(stderr, "\t-G group-id (for root directory)\n");
	fprintf(stderr, "\t-U user-id (for root directory)\n");
	fprintf(stderr, "\t-M octal access-mask (for root directory)\n");
	fprintf(stderr, "\t-b allocation block size (4096 optimal)\n");
	fprintf(stderr, "\t-c clump size list (comma separated)\n");
	fprintf(stderr, "\t\ta=blocks (attributes file)\n");
	fprintf(stderr, "\t\tb=blocks (bitmap file)\n");
	fprintf(stderr, "\t\tc=blocks (catalog file)\n");
	fprintf(stderr, "\t\td=blocks (user data fork)\n");
	fprintf(stderr, "\t\te=blocks (extents file)\n");
	fprintf(stderr, "\t\tr=blocks (user resource fork)\n");
	fprintf(stderr, "\t-i starting catalog node id\n");
	fprintf(stderr, "\t-n b-tree node size list (comma separated)\n");
	fprintf(stderr, "\t\te=size (extents b-tree)\n");
	fprintf(stderr, "\t\tc=size (catalog b-tree)\n");
	fprintf(stderr, "\t\ta=size (attributes b-tree)\n");
	fprintf(stderr, "\t-v volume name (in ascii or UTF-8)\n");

	fprintf(stderr, "  examples:\n");
	fprintf(stderr, "\t%s -v Untitled /dev/rdisk0s7 \n", progname);
	fprintf(stderr, "\t%s -v Untitled -n c=4096,e=1024 /dev/rdisk0s7 \n", progname);
	fprintf(stderr, "\t%s -v Untitled -c b=64,c=1024 /dev/rdisk0s7 \n\n", progname);

	exit(1);
}
