CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -g -MMD -MP
LDFLAGS = -pthread

SRC = main.cpp tracker_client.cpp seeder.cpp file_registry.cpp downloader.cpp download_manager.cpp ../common/network_utils.cpp ../common/tracker_info.cpp ../common/file_chunker.cpp ../common/sha1.cpp
OBJ = $(SRC:.cpp=.o)
TARGET = client

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
