CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -g -MMD -MP
LDFLAGS = -pthread

SRC = main.cpp connection_handler.cpp tracker_state.cpp replicator.cpp ../common/network_utils.cpp ../common/tracker_info.cpp ../common/sha1.cpp
OBJ = $(SRC:.cpp=.o)
TARGET = tracker

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) $(TARGET)

# Rebuild objects when a header they include changes (from -MMD).
-include $(OBJ:.o=.d)

.PHONY: all clean
