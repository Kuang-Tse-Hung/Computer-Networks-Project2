# Compiler and Flags
CC      = gcc
LD      = gcc
CFLAGS  = -Wall -g -std=c11

LDFLAGS =
DEFS    =

# Target Executables
TARGETS = sendfile recvfile

# Source Files
SENDFILE_SRC = sendfile.c packet.c
RECVFILE_SRC = recvfile.c packet.c

all: $(TARGETS)

sendfile: $(SENDFILE_SRC)
	$(CC) $(DEFS) $(CFLAGS) $(LDFLAGS) -o sendfile $(SENDFILE_SRC)

recvfile: $(RECVFILE_SRC)
	$(CC) $(DEFS) $(CFLAGS) $(LDFLAGS) -o recvfile $(RECVFILE_SRC)

clean:
	rm -f *.o
	rm -f *~
	rm -f core.*
	rm -f $(TARGETS)
