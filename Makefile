all: printersim printsched

printersim: printersim.c
	gcc printersim.c gfx.c -o printersim -Wall -g -pthread -lX11 -lm

printsched: printsched.c
	gcc printsched.c gfx.c -o printsched -Wall -g -pthread -lX11 -lm

clean:
	rm -f printersim printsched
