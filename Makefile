CC = gcc
CFLAGS += -Wall -pedantic
OBJS = mailfail.o

mailfail: $(OBJS)
	$(CC) -o mailfail $(OBJS)

mailfail.o: mailfail.c conf.h

.PHONY: clean

clean:
	rm -f $(OBJS) mailfail
