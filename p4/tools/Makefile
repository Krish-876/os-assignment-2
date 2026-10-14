CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2

TARGET = tier
SRC = tier.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)

.PHONY: all clean install
