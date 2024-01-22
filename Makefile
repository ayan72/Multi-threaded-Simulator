all: a4w23

a4w23: a4w23.o
	g++ a4w23.cpp -o a4w23 -pthread

clean:
	rm -f a4w23 a4w23 *.o *.tar

tar:
	tar -cvf abbas-a4.tar *.cpp Makefile inputFile report.pdf