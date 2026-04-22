.PHONY: all
code: main.c buddy.c
	gcc -o code main.c buddy.c