#define _XOPEN_SOURCE 500
#define _GNU_SOURCE

#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#include <openssl/rc4.h>
#include <openssl/md4.h>

static const unsigned char bcache_magic[] = {
	0xc6, 0x85, 0x73, 0xf6, 0x4e, 0x1a, 0x45, 0xca,
	0x82, 0x65, 0xf5, 0x7f, 0x48, 0xba, 0x6d, 0x81 };

unsigned char zero[4096];

#define Pread(fd, buf, size, offset) do {				\
	int _read = 0, _r;						\
	while (_read < size) {						\
		_r = pread(fd, buf, (size) - _read, (offset) + _read);	\
		if (_r <= 0)						\
			goto err;					\
		_read += _r;						\
	}								\
} while (0)

#define Pwrite(fd, buf, size, offset) do {				\
	int _write = 0, _r;						\
	while (_write < size) {						\
		_r = pwrite(fd, buf, (size) - _write, offset + _write);	\
		if (_r < 0)						\
			goto err;					\
		_write += _r;						\
	}								\
} while (0)

/* Marsaglia polar method
 */
double normal()
{
	double x, y, s;
	static double n = 0 / (double) 0;

	if (n == n) {
		x = n;
		n = 0 / (double) 0;
		return x;
	}
	
	do {
		x = random() / (double) (RAND_MAX / 2) - 1;
		y = random() / (double) (RAND_MAX / 2) - 1;

		s = x * x + y * y;
	} while (s >= 1);

	s = sqrt(-2 * log(s) / s);
	n =	y * s;
	return  x * s;
}

long getblocks(int fd)
{
	long ret;
	struct stat statbuf;
	if (fstat(fd, &statbuf)) {
		perror("stat error");
		exit(EXIT_FAILURE);
	}
	ret = statbuf.st_size / 512;
	if (S_ISBLK(statbuf.st_mode))
		if (ioctl(fd, BLKGETSIZE, &ret)) {
			perror("ioctl error");
			exit(EXIT_FAILURE);
		}
	return ret;
}

struct pagestuff {
	unsigned char csum[16];
	unsigned char oldcsum[16];
	int readcount;
	int writecount;
};

void flushlog(int fd, char *logbuf)
{
	int w = 0, len = klogctl(4, logbuf, 1 << 21);
	if (len == -1) {
		perror("Error reading kernel log");
		exit(EXIT_FAILURE);
	}

	while (w < len) {
		int r = write(fd, logbuf + w, len - w);
		if (r == -1) {
			perror("Error writing log");
			exit(EXIT_FAILURE);
		}
		w += r;
	}
}

int main(int argc, char **argv)
{
	bool walk = false, randsize = false, verbose = false, csum = false, destructive = false, log = false;
	int fd1, fd2 = 0, logfd, direct = 0, nbytes = 4096, j;
	unsigned long size, i, offset = 0, done = 0;
	void *buf1 = NULL, *buf2 = NULL;
	struct pagestuff *pages, *p;
	unsigned char c[16];
	char logbuf[1 << 21];
	time_t last_printed = 0;

	RC4_KEY writedata;
	RC4_set_key(&writedata, 16, bcache_magic);

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "direct") == 0)
			direct = O_DIRECT;
		else if (strcmp(argv[i], "walk") == 0)
			walk = true;
		else if (strcmp(argv[i], "verbose") == 0)
			verbose = true;
		else if (strcmp(argv[i], "size") == 0)
			randsize = true;
		else if (strcmp(argv[i], "csum") == 0)
			csum = true;
		else if (strcmp(argv[i], "write") == 0)
			destructive = true;
		else if (strcmp(argv[i], "log") == 0)
			log = true;
		else
			break;
	}

	if (i + 1 > argc) {
		printf("Please enter a device to test\n");
		exit(EXIT_FAILURE);
	}

	if (i + 2 > argc && !csum) {
		printf("Please enter a device to compare against\n");
		exit(EXIT_FAILURE);
	}

	fd1 = open(argv[i], (destructive ? O_RDWR : O_RDONLY)|direct);
	if (!csum)
		fd2 = open(argv[i + 1], (destructive ? O_RDWR : O_RDONLY)|direct);

	if (fd1 == -1 || fd2 == -1) {
		perror("Error opening device");
		exit(EXIT_FAILURE);
	}

	size = getblocks(fd1);
	if (!csum)
		size = MIN(size, getblocks(fd2));

	size = size / 8 - 16;
	pages = calloc(size + 16, sizeof(*pages));
	printf("size %li\n", size);

	if (posix_memalign(&buf1, 4096, 4096 * 16) ||
	    posix_memalign(&buf2, 4096, 4096 * 16)) {
		printf("Could not allocate buffers\n");
		exit(EXIT_FAILURE);
	}
	//setvbuf(stdout, NULL, _IONBF, 0);
	
	if (log) {
		logfd = open("log", O_WRONLY|O_CREAT|O_TRUNC, 0644);
		if (logfd == -1) {
			perror("Error opening log file");
			exit(EXIT_FAILURE);
		}
		klogctl(8, 0, 6);
	}

	for (i = 0;; i++) {
		bool writing = destructive && (i & 1);
		nbytes = randsize ? drand48() * 16 + 1 : 1;
		nbytes <<= 12;

		offset >>= 12;
		offset += walk ? normal() * 20 : random();
		offset %= size;
		offset <<= 12;

		if (log && !(i % 200))
			flushlog(logfd, logbuf);

		if (!verbose) {
			time_t now = time(NULL);
			if (now - last_printed >= 2) {
				last_printed = now;
				goto print;
			}
		} else
print:			printf("Loop %6li offset %9li sectors %3i, %6lu mb done\n",
			       i, offset >> 9, nbytes >> 9, done >> 11);

		done += nbytes >> 9;

		if (!writing)
			Pread(fd1, buf1, nbytes, offset);
		if (!writing && !csum)
			Pread(fd2, buf2, nbytes, offset);

		for (j = 0; j < nbytes; j += 4096) {
			p = &pages[(offset + j) / 4096];

			if (writing)
				RC4(&writedata, 4096, zero, buf1 + j);

			if (csum) {
				MD4(buf1 + j, 4096, &c[0]);

				if (writing ||
				    (!p->readcount && !p->writecount)) {
					memcpy(&p->oldcsum[0], &p->csum[0], 16);
					memcpy(&p->csum[0], c, 16);
				} else if (memcmp(&p->csum[0], c, 16))
					goto bad;
			} else if (!writing &&
				   memcmp(buf1 + j,
					  buf2 + j,
					  4096))
				goto bad;

			writing ? p->writecount++ : p->readcount++;
		}
		if (writing)
			Pwrite(fd1, buf1, nbytes, offset);
		if (writing && !csum)
			Pwrite(fd2, buf2, nbytes, offset);
	}
err:
	perror("IO error");
	flushlog(logfd, logbuf);
	exit(EXIT_FAILURE);
bad:
	printf("Bad read! loop %li offset %li readcount %i writecount %i\n",
	       i, (offset + j) >> 9, p->readcount, p->writecount);

	if (!memcmp(&p->oldcsum[0], c, 16))
		printf("Matches previous csum\n");

	flushlog(logfd, logbuf);
	exit(EXIT_FAILURE);
}
