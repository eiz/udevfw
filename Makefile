udevfw: udevfw.c
	cc -o udevfw udevfw.c MurmurHash2.c -ludev -Wall -lpthread

clean:
	rm -f udevfw
