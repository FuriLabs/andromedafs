CC = gcc

CFLAGS = `pkg-config fuse3 --cflags`
LDFLAGS = `pkg-config fuse3 --libs`

SOURCES = andromedafs.c
TARGET = andromedafs

PREFIX ?= /usr

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(SOURCES) -o $(TARGET) $(CFLAGS) $(LDFLAGS)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/sbin
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/sbin/

clean:
	rm -f $(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/sbin/$(TARGET)
