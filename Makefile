udevfw: udevfw.c
	cc -o udevfw udevfw.c MurmurHash2.c -ludev

clean:
	rm -f udevfw
