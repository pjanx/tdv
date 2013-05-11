SHELL = /bin/sh

pkgs = ncursesw glib-2.0 gio-2.0
tests = test-stardict
targets = sdtui add-pronunciation $(tests)

CFLAGS = -ggdb -std=gnu99 -Wall -Wextra -Wno-missing-field-initializers \
		 `pkg-config --cflags $(pkgs)`
LDFLAGS = `pkg-config --libs $(pkgs)`

.PHONY: all clean test

all: $(targets)

clean:
	rm -f $(targets) src/*.o

sdtui: src/sdtui.o src/stardict.o
	$(CC) $^ -o $@ $(LDFLAGS)

add-pronunciation: src/add-pronunciation.o src/stardict.o src/generator.o
	$(CC) $^ -o $@ $(LDFLAGS)

test-stardict: src/test-stardict.o src/stardict.o src/generator.o
	$(CC) $^ -o $@ $(LDFLAGS)

test: $(tests)
	for i in $(tests); do         \
		gtester --verbose ./$$i;  \
	done

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
