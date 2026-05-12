CFLAGS   += -Wall -Wextra -Wno-unused-result -std=c11 -O2 -pthread

CFLAGS   += $(shell pkg-config --cflags gtk+-3.0 ayatana-appindicator3-0.1 libnotify)
CFLAGS   += $(shell curl-config --cflags)
LDFLAGS  += $(shell pkg-config --libs gtk+-3.0 ayatana-appindicator3-0.1 libnotify)
LDFLAGS  += $(shell curl-config --libs)

SRCDIR = src
SRC = $(SRCDIR)/main.c $(SRCDIR)/config.c $(SRCDIR)/menu.c $(SRCDIR)/state.c $(SRCDIR)/server.c $(SRCDIR)/curl.c $(SRCDIR)/child.c $(SRCDIR)/notify.c $(SRCDIR)/globals.c

OBJ = $(SRC:.c=.o)
TARGET = llm-daemon

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

%.o: %.c globals.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ)

clean-all:
	rm -f $(OBJ) $(TARGET)