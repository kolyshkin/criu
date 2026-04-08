#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "zdtmtst.h"

const char *test_doc = "Test soft-dirty vma flags propagated after merging vma-s.";
const char *test_author = "Andrei Vagin";

char *filename;
TEST_OPTION(filename, string, "file name", 1);

int main(int argc, char **argv)
{
	unsigned char *reg1 = NULL, *reg, *reg2;
	size_t new_size = (1UL<<30);
	bool remapped = false;
	int fd;

	test_init(argc, argv);


	fd = open(filename, O_RDWR | O_CREAT, 0644);
	if (fd == -1) {
		pr_perror("open");
		return 1;
	}
	if (ftruncate(fd, new_size * 2) == -1) {
		pr_perror("ftruncate");
		return 1;
	}

	reg = mmap(NULL, new_size * 2, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reg == MAP_FAILED) {
		pr_perror("mmap new");
		return 1;
	}

	reg1 = mmap(reg, new_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_FILE | MAP_FIXED, fd, 0);
	if (reg1 == MAP_FAILED) {
		pr_perror("mmap new");
		return 1;
	}

	reg2 = mmap(NULL, new_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_FILE, fd, new_size);
	if (reg2 == MAP_FAILED) {
		pr_perror("mmap new");
		return 1;
	}
	reg2[0] = 1;
	test_daemon();

	while (test_go()) {
		if (test_wait_pre_dump())
			goto skip;
		if (remapped) {
			test_wait_pre_dump_ack();
			continue;
		}

		reg2 = mremap(reg2, new_size, new_size,
			      MREMAP_MAYMOVE | MREMAP_FIXED, reg+new_size);
		if (reg2 == MAP_FAILED) {
			pr_perror("mmap new");
			return 1;
		}

		test_wait_pre_dump_ack();
		remapped = true;
	}

skip:
	test_waitsig();


	pass();
	return 0;
}
