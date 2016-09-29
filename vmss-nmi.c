/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2016, Joyent, Inc. All rights reserved.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <strings.h>
#include <stdarg.h>
#include <unistd.h>
#include <alloca.h>

/*
 * A simple (and therefore, surely brittle) program to process a VMware
 * suspended state (VMSS) file to post a non-maskable interrupt (NMI)
 * onto it.
 */
#define	VMSS_MAGIC_OLD		0xbed0bed0
#define	VMSS_MAGIC_RESTORED	0xbed1bed1
#define	VMSS_MAGIC		0xbed2bed2
#define	VMSS_MAGIC_PARTIAL	0xbed3bed3

typedef uint16_t vmss_tag_t;
typedef uint32_t vmss_index_t;

/*
 * A tag has a name length, a number of indices, and a value size field.
 */
#define	VMSS_TAG_NAMELEN_MASK		0xff
#define	VMSS_TAG_NAMELEN_SHIFT		8
#define	VMSS_TAG_NINDX_MASK		3
#define	VMSS_TAG_NINDX_SHIFT		6
#define	VMSS_TAG_VALSIZE_MASK		0x3f
#define	VMSS_TAG_VALSIZE_SHIFT		0

#define	VMSS_TAG_NAMELEN_MASK		0xff

#define	VMSS_TAG_NAMELEN(t)		\
	(((t) >> VMSS_TAG_NAMELEN_SHIFT) & VMSS_TAG_NAMELEN_MASK)

#define	VMSS_TAG_NINDX(t)		\
	(((t) >> VMSS_TAG_NINDX_SHIFT) & VMSS_TAG_NINDX_MASK)

#define	VMSS_TAG_VALSIZE(t)		\
	(((t) >> VMSS_TAG_VALSIZE_SHIFT) & VMSS_TAG_VALSIZE_MASK)

#define	VMSS_TAG_NULL			((vmss_tag_t)0)

/*
 * Special size values to denote a block and to denote a compressed block.
 */
#define	VMSS_TAG_VALSIZE_BLOCK_COMPRESSED	0x3e
#define	VMSS_TAG_VALSIZE_BLOCK			0x3f

#define	VMSS_TAG_ISBLOCK(t)		\
	(VMSS_TAG_VALSIZE(t) == VMSS_TAG_VALSIZE_BLOCK_COMPRESSED || \
	VMSS_TAG_VALSIZE(t) == VMSS_TAG_VALSIZE_BLOCK)

typedef struct vmss_header {
	uint32_t	vmss_hdr_id;
	uint32_t	vmss_hdr_version;
	uint32_t	vmss_hdr_numgroups;
} vmss_header_t;

#define	VMSS_GROUP_NAMELEN		64

typedef struct vmss_group {
	char		vmss_group_name[VMSS_GROUP_NAMELEN];
	uint64_t	vmss_group_offs;
	uint64_t	vmss_group_size;
} vmss_group_t;

typedef struct vmss_block {
	uint64_t	vmss_block_size;
	uint64_t	vmss_block_memsize;
} vmss_block_t;

typedef uint16_t vmss_block_padsize_t;

static const char *g_cmd = "vmss-nmi";
static int g_verbose = 0;
static int g_cpu = 0;
static uint8_t g_nmi = 1;

static void
usage()
{
	(void) fprintf(stderr, "Usage: %s [-c cpu] [-n] [-v] [-z] vmss-file\n"
	    "\n"
	    "  -c cpu   Set pendingNMI only on specified CPU\n"
	    "  -n       Display but don't alter pendingNMI\n"
	    "  -v       Verbose output\n"
	    "  -z       Zero out pendingNMI rather than set it\n"
	    "\n", g_cmd);

	exit(EXIT_FAILURE);
}

static void
fatal(char *fmt, ...)
{
	va_list ap;
	int error = errno;

	va_start(ap, fmt);

	(void) fprintf(stderr, "%s: ", g_cmd);
	(void) vfprintf(stderr, fmt, ap);

	if (fmt[strlen(fmt) - 1] != '\n')
		(void) fprintf(stderr, ": %s\n", strerror(error));

	exit(EXIT_FAILURE);
}

static void
verbose(char *fmt, ...)
{
	if (!g_verbose)
		return;

	va_list ap;

	va_start(ap, fmt);

	(void) fprintf(stdout, "%s: ", g_cmd);
	(void) vfprintf(stdout, fmt, ap);
	(void) fprintf(stdout, "\n");
}

static void
warn(char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	(void) fprintf(stderr, "%s: ", g_cmd);
	(void) vfprintf(stderr, fmt, ap);
	(void) fprintf(stderr, "\n");
}

static void
setnmi(FILE *fp, vmss_group_t *grp)
{
	if (fseek(fp, grp->vmss_group_offs, SEEK_SET) == -1) {
		fatal("couldn't read group %s at offset 0x%llx",
		    grp->vmss_group_name, grp->vmss_group_offs);
	}

	for (;;) {
		vmss_tag_t tag;
		char name[VMSS_TAG_NAMELEN_MASK + 1];
		char buf[VMSS_TAG_VALSIZE_MASK];
		vmss_index_t idx[VMSS_TAG_NINDX_MASK];
		int len, size, nindx;

		if (fread(&tag, sizeof (tag), 1, fp) != 1)
			fatal("couldn't read tag at offset 0x%x", ftell(fp));

		if (tag == VMSS_TAG_NULL)
			break;

		len = VMSS_TAG_NAMELEN(tag);
		nindx = VMSS_TAG_NINDX(tag);
		size = VMSS_TAG_VALSIZE(tag);

		/*
		 * Read the name, which can be of most TAG_NAMELEN_MASK bytes.
		 */
		if (fread(name, len, 1, fp) != 1)
			fatal("couldn't read name at offset %x", ftell(fp));

		name[len] = '\0';

		bzero(idx, sizeof (idx));

		if (fread(idx, sizeof (vmss_index_t), nindx, fp) != nindx)
			fatal("couldn't read index at offset %x", ftell(fp));

		verbose("tag %-30s size %3d nindx %d ([%d][%d][%d])", name,
		    size, nindx, idx[0], idx[1], idx[2]);

		if (VMSS_TAG_ISBLOCK(tag)) {
			vmss_block_t blk;
			vmss_block_padsize_t pad;

			off_t offs = ftell(fp);

			if (fread(&blk, sizeof (blk), 1, fp) != 1)
				fatal("couldn't read block at 0x%x", offs);

			/*
			 * Amazingly (and ironically) VMSS stores the block
			 * size padding in such a way that it can't be read
			 * into an unpacked structure -- so the padding has
			 * to be read separately.  Slow clap!
			 */
			if (fread(&pad, sizeof (pad), 1, fp) != 1)
				fatal("couldn't read padding at 0x%x", offs);

			verbose("  block size %d, memsize %d, pad %d",
			    blk.vmss_block_size, blk.vmss_block_memsize, pad);

			if (fseek(fp,
			    blk.vmss_block_size + pad, SEEK_CUR) == -1)
				fatal("unable to skip block at 0x%x", offs);

			continue;
		}

		if (strcmp(name, "pendingNMI") != 0) {
			if (fseek(fp, size, SEEK_CUR) == -1)
				fatal("couldn't seek at 0x%x", ftell(fp));
			continue;
		}

		if (size != 1) {
			fatal("found pendingNMI size to be unexpected "
			    "value of %d (expected 1)", size);
		}

		if (fread(buf, size, 1, fp) != 1)
			fatal("couldn't read buffer at offset %x", ftell(fp));

		if (idx[0] != g_cpu) {
			if (g_cpu == -1) {
				warn("pendingNMI for CPU %d is %d",
				    idx[0], *((uint8_t *)buf), g_cpu);
				continue;
			}

			warn("pendingNMI for CPU %d is %d; skipping (target "
			    "CPU is %d)", idx[0], *((uint8_t *)buf), g_cpu);
			continue;
		}

		warn("pendingNMI for CPU %d is %d; setting to %d",
		    idx[0], *((uint8_t *)buf), g_nmi);

		if (fseek(fp, -1, SEEK_CUR) == -1)
			fatal("couldn't reset offset");

		buf[0] = g_nmi;

		if (fwrite(buf, size, 1, fp) != 1)
			fatal("couldn't write buffer at offset %x", ftell(fp));
	}
}

static void
process(char *filename)
{
	vmss_header_t hdr;
	vmss_group_t *groups;
	FILE *fp;
	uint32_t ngroups, i;

	if ((fp = fopen(filename, "r+")) == NULL)
		fatal("can't open %s", filename);

	if (fread(&hdr, sizeof (hdr), 1, fp) != 1)
		fatal("couldn't read VMSS header");

	switch (hdr.vmss_hdr_id) {
	case VMSS_MAGIC_OLD:
		fatal("can't read 32-bit VMSS file\n");
		/*NOTREACHED*/

	case VMSS_MAGIC:
	case VMSS_MAGIC_RESTORED:
	case VMSS_MAGIC_PARTIAL:
		break;

	default:
		fatal("%s not recognized as a VMSS file\n", filename);
	}

	verbose("VMSS version %d, %d groups",
	    hdr.vmss_hdr_version, hdr.vmss_hdr_numgroups);

	ngroups = hdr.vmss_hdr_numgroups;
	groups = alloca(ngroups * sizeof (vmss_group_t));

	if (fread(groups, sizeof (vmss_group_t), ngroups, fp) != ngroups)
		fatal("couldn't read %d groups", ngroups);

	for (i = 0; i < ngroups; i++) {
		vmss_group_t *grp = &groups[i];

		verbose("group %3d: %-28s offs=0x%llx size=0x%llx",
		    i, grp->vmss_group_name, grp->vmss_group_offs,
		    grp->vmss_group_size);

		if (strcmp(grp->vmss_group_name, "cpu") == 0)
			setnmi(fp, grp);
	}

	(void) fclose(fp);
}

int
main(int argc, char **argv)
{
	int c;
	char *end;

	while ((c = getopt(argc, argv, "c:nvz")) != -1) {
		switch (c) {
		case 'c':
			g_cpu = strtoul(optarg, &end, 10);

			if (*end != '\0' || g_cpu < 0)
				fatal("invalid CPU '%s'\n", optarg);

			break;

		case 'v':
			g_verbose = 1;
			break;

		case 'z':
			g_nmi = 0;
			break;

		case 'n':
			g_cpu = -1;
			break;

		default:
			usage();
		}
	}

	if (argc == optind)
		fatal("expected a VMSS file\n");

	process(argv[optind]);
}
