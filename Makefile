SHELL = /bin/sh

pkgs = ncursesw glib-2.0 gio-2.0
targets = sdcli

CC = clang
CFLAGS = -ggdb -std=gnu99 -Wall -Wextra -Wno-missing-field-initializers \
		 `pkg-config --cflags $(pkgs)`
LDFLAGS = `pkg-config --libs $(pkgs)`

.PHONY: all clean

all: $(targets)

clean:
	rm -f $(targets) *.o

sdcli: sdcli.o stardict.o
	$(CC) $^ -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
