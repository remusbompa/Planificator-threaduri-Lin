CC = gcc
CFLAGS = -Wall -g -fPIC

build: libscheduler.so

libscheduler.so: so_scheduler.o
	$(CC) -shared $< -o $@

so_scheduler.o: so_scheduler.c
	$(CC) $(CFLAGS) -c $<

.PHONY: clean

clean:
	rm -f *.o *~ libscheduler.so
