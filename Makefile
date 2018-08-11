CXXFLAGS+=-std=c++11

noodly: noodly.o microorb.o
	g++ -o $@ $^ -lspixels -lMPR121 -lwiringPi -lusb

clean:
	rm -f noodly noodly.o
