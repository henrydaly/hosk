ROOT = ../..

include $(ROOT)/common/Makefile.common

BINS = $(BINDIR)/lockfree-hosk-skiplist
CXX = g++

.PHONY:	all clean

all:	main

allocator.o: allocator.h
	$(CXX) $(CFLAGS) -c -o $(BUILDIR)/allocator.o allocator.cpp -std=c++11 -I -lnuma.
	
skiplist.o: skiplist.h allocator.h
	$(CXX) $(CFLAGS) -c -o $(BUILDIR)/skiplist.o skiplist.cpp -std=c++11 -I.
	
enclave.o: enclave.h skiplist.h 
	$(CXX) $(CFLAGS) -c -o $(BUILDIR)/enclave.o enclave.cpp -std=c++11 -I.
	
helper.o: helper.h skiplist.h enclave.h
	$(CXX) $(CFLAGS) -c -o $(BUILDIR)/helper.o helper.cpp -std=c++11 -I.
	
application.o: skiplist.h enclave.h
	$(CXX) $(CFLAGS) -c -o $(BUILDIR)/application.o application.cpp -std=c++11 -I.

test.o: skiplist.h enclave.h allocator.h
	$(CXX) $(CFLAGS) -c -o $(BUILDIR)/test.o test.cpp -std=c++11 -I.
	
main: skiplist.o enclave.o application.o test.o helper.o allocator.o
	$(CXX) $(CFLAGS) $(BUILDIR)/allocator.o $(BUILDIR)/skiplist.o $(BUILDIR)/enclave.o $(BUILDIR)/helper.o $(BUILDIR)/application.o $(BUILDIR)/test.o -o $(BINS) -std=c++11 $(LDFLAGS) -I. -lnuma
	
clean:
	-rm -f $(BINS)
