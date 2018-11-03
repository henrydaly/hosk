ROOT = ../..

include $(ROOT)/common/Makefile.common

BINS = $(BINDIR)/lockfree-hosk-skiplist
CXX = g++
ARGS = -c -o

.PHONY:	all clean

all:	main

allocator.o: allocator.h
	$(CXX) $(CFLAGS) ${ARGS} $(BUILDIR)/allocator.o allocator.cpp -std=c++11 -I -lnuma.
	
skiplist.o: allocator.h skiplist.h
	$(CXX) $(CFLAGS) ${ARGS} $(BUILDIR)/skiplist.o skiplist.cpp -std=c++11 -I.
	
enclave.o: enclave.h skiplist.h 
	$(CXX) $(CFLAGS) ${ARGS} $(BUILDIR)/enclave.o enclave.cpp -std=c++11 -I.
	
helper.o enclave.h: skiplist.h
	$(CXX) $(CFLAGS) ${ARGS} $(BUILDIR)/helper.o helper.cpp -std=c++11 -I.
	
application.o: enclave.h skiplist.h
	$(CXX) $(CFLAGS) ${ARGS} $(BUILDIR)/application.o application.cpp -std=c++11 -I.

test.o: allocator.h enclave.h skiplist.h
	$(CXX) $(CFLAGS) ${ARGS} $(BUILDIR)/test.o test.cpp -std=c++11 -I.
	
main: skiplist.o enclave.o application.o test.o helper.o allocator.o
	$(CXX) $(CFLAGS) $(BUILDIR)/allocator.o $(BUILDIR)/skiplist.o $(BUILDIR)/enclave.o $(BUILDIR)/helper.o $(BUILDIR)/application.o $(BUILDIR)/test.o -o $(BINS) -std=c++11 $(LDFLAGS) -I. -lnuma
	
clean:
	-rm -f $(BINS)
