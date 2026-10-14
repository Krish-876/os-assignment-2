CC     = gcc
CFLAGS = -Wall -Wextra -O2 -pthread

all: prog_a_cpu prog_b_io

prog_a_cpu: prog_a_cpu.c workload_cpu.h
	$(CC) $(CFLAGS) -o $@ $<

prog_b_io: prog_b_io.c workload_io.h
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f prog_a_cpu prog_b_io
