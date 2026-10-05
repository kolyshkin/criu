/*
 * Standalone copy of test_merge() from upstream kernel selftest
 * tools/testing/selftests/mm/soft-dirty.c (commit c7ba92bcfea3),
 * which tests the fix 6707915e030a ("mm: propagate VM_SOFTDIRTY on merge").
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

#define PM_SOFT_DIRTY (1ULL << 55)

static int pagemap_fd, pagesize, failed;

static void die(const char *msg)
{
	perror(msg);
	exit(2);
}

static void clear_softdirty(void)
{
	int fd = open("/proc/self/clear_refs", O_WRONLY);

	if (fd < 0 || write(fd, "4", 1) != 1)
		die("clear_refs");
	close(fd);
}

static int is_softdirty(void *addr)
{
	uint64_t ent;
	off_t off = (uintptr_t)addr / pagesize * sizeof(ent);

	if (pread(pagemap_fd, &ent, sizeof(ent), off) != sizeof(ent))
		die("pread pagemap");
	return !!(ent & PM_SOFT_DIRTY);
}

static void check(int ok, const char *name)
{
	printf("%s: %s\n", ok ? "ok" : "FAIL", name);
	if (!ok)
		failed = 1;
}

int main(void)
{
	char *reserved, *map, *map2;

	pagesize = getpagesize();
	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	if (pagemap_fd < 0)
		die("open pagemap");

	reserved = mmap(NULL, 5 * pagesize, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (reserved == MAP_FAILED)
		die("mmap");
	munmap(reserved, 4 * pagesize);

	/* mremap merge */
	map = mmap(&reserved[pagesize], pagesize, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
	if (map == MAP_FAILED)
		die("mmap");
	clear_softdirty();
	map2 = mmap(&reserved[3 * pagesize], pagesize, PROT_READ | PROT_WRITE,
		    MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
	if (map2 == MAP_FAILED)
		die("mmap");
	map2 = mremap(map2, pagesize, pagesize, MREMAP_FIXED | MREMAP_MAYMOVE,
		      &reserved[2 * pagesize]);
	if (map2 == MAP_FAILED)
		die("mremap");
	check(is_softdirty(map), "soft-dirty after remap merge 1st pg");
	check(is_softdirty(map2), "soft-dirty after remap merge 2nd pg");
	munmap(map, 2 * pagesize);

	/* mprotect merge */
	map = mmap(&reserved[pagesize], pagesize, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
	if (map == MAP_FAILED)
		die("mmap");
	clear_softdirty();
	map2 = mmap(&reserved[2 * pagesize], pagesize, PROT_READ | PROT_WRITE | PROT_EXEC,
		    MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
	if (map2 == MAP_FAILED)
		die("mmap");
	if (mprotect(map, pagesize, PROT_READ | PROT_WRITE | PROT_EXEC))
		die("mprotect");
	check(is_softdirty(map), "soft-dirty after mprotect merge 1st pg");
	check(is_softdirty(map2), "soft-dirty after mprotect merge 2nd pg");

	return failed;
}
