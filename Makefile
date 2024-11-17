CC = gcc
CFLAGS = -Wall -Wextra

all: rfind rfind_server

rfind: rfind.c
	$(CC) $(CFLAGS) -o rfind rfind.c

rfind_server: rfind_server.c
	$(CC) $(CFLAGS) -o rfind_server rfind_server.c

clean:
	rm -f rfind rfind_server