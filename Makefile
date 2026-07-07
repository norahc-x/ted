CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c99 -Wall -Wextra -pedantic -Werror
SRC      = main.c terminal.c buffer.c editor.c tree.c syntax.c
OBJ      = $(SRC:.c=.o)

ted: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

$(OBJ): editor.h

# Instrumented build: run it, then `make clean ted` to restore the normal one.
asan:
	$(MAKE) clean
	$(MAKE) ted CFLAGS="-std=c99 -Wall -Wextra -pedantic -Werror -O1 -g -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined"

memcheck: ted
	valgrind --leak-check=full --show-leak-kinds=all ./ted

clean:
	rm -f ted $(OBJ)

.PHONY: asan memcheck clean
