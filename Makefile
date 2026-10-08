CC ?= cc

CFLAGS  += -Wall -Wextra -O2
CPPFLAGS += -I/usr/local/include
LDFLAGS  += -L/usr/local/lib
LDLIBS   += -lX11

TARGET = xdragon
SRC    = src/main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -d "$(DESTDIR)/usr/local/bin"
	install -m 755 $(TARGET) "$(DESTDIR)/usr/local/bin/$(TARGET)"

.PHONY: all clean install


