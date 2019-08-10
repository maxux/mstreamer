EXEC = mstreamer

SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

CFLAGS   = -W -Wall -O2 -pipe -g -std=gnu99
LDFLAGS  = -lSDL2 -lSDL2_image

all: $(EXEC)

$(EXEC): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) -c $(CFLAGS) $*.c

clean:
	rm -fv *.o

mrproper: clean
	rm -fv $(EXEC)
