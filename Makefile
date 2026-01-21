# Variáveis
CC = gcc
CFLAGS = -Wall -Wextra -g
SRC = main.c myfs.c disk.c inode.c util.c vfs.c
OBJ = $(SRC:.c=.o)
EXEC = myfs

# Regras
all: $(EXEC)

$(EXEC): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(EXEC)

run: all
	./$(EXEC)