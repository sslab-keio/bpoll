OBJ := ixgbevf.o
SRCS := ixgbevf.c

$(OBJ): $(SRCS)
	cc $(DRIVER_CFLAGS) -c -o $@ $^

.PHONY: clean
clean:
	rm -f $(OBJ)
