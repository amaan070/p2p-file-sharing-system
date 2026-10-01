# Unit tests (fast, self-contained). Integration / stress scripts are *.sh in
# this directory and are run by hand; see the README, "Testing".
#
#   make        build every unit test
#   make run    build and run every unit test
#   make clean  remove everything built here

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -g -I..

TRACKER_SRCS = ../tracker/tracker_state.cpp ../common/sha1.cpp
CLIENT_SRCS  = ../client/seeder.cpp ../client/file_registry.cpp ../common/file_chunker.cpp \
               ../common/sha1.cpp ../common/network_utils.cpp
DOWNLOAD_SRCS = ../client/downloader.cpp $(CLIENT_SRCS) ../common/tracker_info.cpp

UNIT_TESTS = test_protocol test_sha1 test_file_chunker test_tracker_state \
             test_tracker_state_files test_tracker_sync test_password \
             test_seeder_shutdown test_downloader test_downloader_multipeer
TOOLS = tcp_proxy

all: $(UNIT_TESTS) $(TOOLS)

test_protocol: test_protocol.cpp ../common/network_utils.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@

test_sha1: test_sha1.cpp ../common/sha1.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@

test_file_chunker: test_file_chunker.cpp ../common/file_chunker.cpp ../common/sha1.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@

test_tracker_state: test_tracker_state.cpp $(TRACKER_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@

test_tracker_state_files: test_tracker_state_files.cpp $(TRACKER_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@

test_tracker_sync: test_tracker_sync.cpp $(TRACKER_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@

test_password: test_password.cpp $(TRACKER_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@

test_seeder_shutdown: test_seeder_shutdown.cpp $(CLIENT_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@

test_downloader: test_downloader.cpp $(DOWNLOAD_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@

test_downloader_multipeer: test_downloader_multipeer.cpp $(DOWNLOAD_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@

# Test tool (not a test): a TCP relay that can simulate a network partition.
tcp_proxy: tcp_proxy.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@

run: $(UNIT_TESTS)
	@set -e; for t in $(UNIT_TESTS); do echo "== $$t"; ./$$t > /tmp/$$t.out 2>&1 || { cat /tmp/$$t.out; echo "FAILED: $$t"; exit 1; }; tail -1 /tmp/$$t.out; done; echo; echo "All unit tests passed."

clean:
	rm -f $(UNIT_TESTS) $(TOOLS) *.o *.d

.PHONY: all run clean
