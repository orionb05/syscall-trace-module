CC = gcc
CFLAGS = -O2 -Wall

SRC = sysgen.c

all: sysgen_read sysgen_mmap sysgen_futex sysgen_openat

sysgen_read:
	$(CC) $(CFLAGS) -DGEN_READ   -o $@ $(SRC)

sysgen_mmap:
	$(CC) $(CFLAGS) -DGEN_MMAP   -o $@ $(SRC)

sysgen_futex:
	$(CC) $(CFLAGS) -DGEN_FUTEX  -o $@ $(SRC)

sysgen_openat:
	$(CC) $(CFLAGS) -DGEN_OPENAT -o $@ $(SRC)

clean:
	rm -f sysgen_read sysgen_mmap sysgen_futex sysgen_openat