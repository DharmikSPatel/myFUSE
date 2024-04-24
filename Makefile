CC=gcc
TESTFLAGS = -Werror -Wno-unused-variable
CFLAGS=-g -Wall -D_FILE_OFFSET_BITS=64
LDFLAGS=-lfuse -lm

OBJ=rufs.o block.o

%.o: %.c
	$(CC) -c $(CFLAGS) $(TESTFLAGS) $< -o $@

rufs: $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o rufs

check_mt:
	findmnt | grep dsp187

remove_mt:
	fusermount -u /tmp/dsp187/mountdir

run_fuse:
	./rufs -s -d /tmp/dsp187/mountdir


.PHONY: clean
clean:
	rm -f *.o rufs
	rm DISKFILE


