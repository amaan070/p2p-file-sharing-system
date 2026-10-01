#ifndef CLIENT_SEEDER_H
#define CLIENT_SEEDER_H

#include <string>

#include "file_registry.h"

// Starts a listener on `ip`:`port` in a background thread. For each
// incoming peer connection, serves MSG_PIECE_REQUEST messages by looking up
// the requested (group_id, filename) in `registry`, reading the requested
// piece from disk, and responding with MSG_PIECE_DATA (or an error
// response if the file/piece isn't available). One connection can be used
// for multiple sequential piece requests.
//
// Returns true if the listener was successfully bound and started, false
// on a socket/bind/listen failure. This function returns immediately; the
// listener thread runs until stop_all_seeders() is called (or the process ends).
bool start_seeder(const std::string& ip, int port, FileRegistry* registry);

// Stops every seeder started with start_seeder(): closes the listening
// sockets, disconnects every peer being served, and BLOCKS until all seeder
// threads have exited. After it returns nothing will touch the FileRegistry
// any more, so the caller may safely destroy it. Safe to call more than once.
void stop_all_seeders();

#endif // CLIENT_SEEDER_H
