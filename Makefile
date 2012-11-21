map:map.o fdevent.o
	gcc -g -O0 -o map map.o fdevent.o

%.o:%.c
	gcc -g -O0 -std=gnu99 -c $< -o $@ 

.PHONY:clean
clean:
	@rm -rf map
	@rm -rf *.o
