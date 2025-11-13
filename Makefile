CC=gcc
CFLAGS=-O2 -Wall -Wextra -pthread

all: server client

server: server.c proto.h
	$(CC) $(CFLAGS) server.c -o server

client: client.c proto.h
	$(CC) $(CFLAGS) client.c -o client

clean:
	rm -f server client
