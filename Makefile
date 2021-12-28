
INC=-I./

test : clean
	g++ -g test.cpp $(INC) -pthread -o test

.PHONY : clean
clean :
	rm -rf test

