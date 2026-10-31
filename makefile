CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread -O2
LDFLAGS = -pthread -lrt
TARGET = torneo

all: $(TARGET)

$(TARGET): torneo.cpp
	$(CXX) $(CXXFLAGS) torneo.cpp -o $(TARGET) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET) 6 2 config.txt

valgrind: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes ./$(TARGET) 4 2 config.txt

clean:
	rm -f $(TARGET) mesa_*.log
