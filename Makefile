SHELL = /bin/sh

pkgs = ncursesw glib-2.0 gio-2.0
tests = test-stardict
targets = sdcli add-pronunciation $(tests)

CC = clang
CFLAGS = -ggdb -std=gnu99 -Wall -Wextra -Wno-missing-field-initializers \
		 `pkg-config --cflags $(pkgs)`
LDFLAGS = `pkg-config --libs $(pkgs)`

.PHONY: all clean test

all: $(targets)

clean:
	rm -f $(targets) *.o

sdcli: sdcli.o stardict.o
	$(CC) $^ -o $@ $(LDFLAGS)

add-pronunciation: add-pronunciation.o stardict.o
	$(CC) $^ -o $@ $(LDFLAGS)

test-stardict: test-stardict.o stardict.o
	$(CC) $^ -o $@ $(LDFLAGS)

test: $(tests)
	for i in $(tests); do         \
		gtester --verbose ./$$i;  \
	done

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
