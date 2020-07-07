CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -Wshadow -pthread -O2

all: loadbalancer

loadbalancer: loadbalancer.o job_queue.o
	$(CC) $(CFLAGS) -o loadbalancer loadbalancer.o job_queue.o

loadbalancer.o: loadbalancer.c
	$(CC) $(CFLAGS) -c loadbalancer.c

job_queue.o: queue.h job_queue.c 
	$(CC) $(CFLAGS) -c job_queue.c

clean:
	$(RM) *.o *~

spotless:
	$(RM) loadbalancer *.o *~
