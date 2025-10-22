compile:
	clear
	gcc nameserver.c -o nameserver -lpthread

clean:
	rm -f nameserver
