# Top-level Makefile.
#
#   make          build the tracker and the client
#   make tracker  build only the tracker      (produces tracker/tracker)
#   make client   build only the client       (produces client/client)
#   make test     build and run all unit tests
#   make clean    remove every build product

.PHONY: all tracker client test clean

all: tracker client

tracker:
	$(MAKE) -C tracker

client:
	$(MAKE) -C client

test:
	$(MAKE) -C tests run

clean:
	$(MAKE) -C tracker clean
	$(MAKE) -C client clean
	$(MAKE) -C tests clean
