all: bin/udp_select

dir:
	mkdir bin

bin/udp_select: src/udp_select.c
	gcc -o bin/udp_select -lrt src/udp_select.c

clean:
	rm -rf bin
