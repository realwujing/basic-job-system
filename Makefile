
INC=-I./

test :
	g++ test.cpp $(INC) -pthread -o test

.PHONY : clean
clean :
	rm test

