udevfw: udevfw.c
	cc -o udevfw udevfw.c MurmurHash2.c -ludev -Wall

clean:
	rm -f udevfw
