OBJ = lib.o
CC = gcc
CFLAGS = -g 
CFLAGSXX =

lib : $(OBJ)
	$(CC) $(CFLAGS) $(CFLAGSXX) $(OBJ) -o lib -lpthread 

lib.o : lib.c
	$(CC) $(CFLAGSXX) -c lib.c

