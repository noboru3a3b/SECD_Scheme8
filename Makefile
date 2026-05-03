CXX = g++
#CXX = clang++

CXXFLAGS = -std=c++17 -Wall -Wextra -O2

TARGET = micro_scheme11
SOURCES = micro_scheme11.cpp

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SOURCES)
#	$(CXX) $(CXXFLAGS) -g -o $(TARGET) $(SOURCES)

clean:
	-rm -f $(TARGET) $(TARGET).exe

.PHONY: all clean
