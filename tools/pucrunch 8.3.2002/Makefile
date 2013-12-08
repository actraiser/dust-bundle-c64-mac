LDFLAGS = /usr/lib/bundle1.o

all: pucrunch

pucrunch: pucrunch.c pucrunch.h
	gcc -Wall -funsigned-char pucrunch.c -o pucrunch -O -lm -Dstricmp=strcasecmp #${LDFLAGS}
