####################################################################
# 
# Makefile for rptr-ctrl-pi - A minimalist repeater controller 
# program written in C for the Raspberry PI. Based on an Arduino
# sketch. Requries the bcm2835 gpio libs. 
#
# (C) 2015 KB4OID Labs, A division of Kodetroll Heavy Industries
#
# Author: Kodetroll
#

CC=gcc
CFLAGS=-I.
LDFLAGS=-lbcm2835 -lrt
#DEPS = C.h
OBJ = rptrctrl.o 

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

all: rptrctrl 

rptrctrl: $(OBJ)
	gcc -o $@ $^ $(CFLAGS) $(LDFLAGS)

.PHONY: clean

cleanall:
	rm -f *.o *~ core rptrctrl 

clean:
	rm -f *.o *~ core 

