

atto: atto.c
	$(CC) atto.c -o atto -Wall -Wextra -pedantic -std=c99

clean: atto
	rm atto