all: 1m-block

1m-block: main.o
	gcc -o 1m-block main.o -lnetfilter_queue

main.o: main.c