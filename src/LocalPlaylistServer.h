#pragma once

// A minimal, loopback-only HTTP server that exists for exactly one reason:
// PVR_STREAM_PROPERTY_STREAMURL's pipe-delimited "url|option=value" syntax
// is parsed by Kodi's own CURL class (xbmc/URL.cpp), which hard-requires
// the literal substring "://" to recognise a protocol at all
// (CURL::Parse(): `strURL.find("://")`). A standard `data:` URI -- the
// first approach tried for serving a rewritten, definite-VOD-shaped copy
// of an in-progress recording's HLS playlist (see
// DispatcharrClient::GetInProgressRecordingStreamUrl()'s comment for why
// that rewrite is needed at all) -- has no "://" anywhere in it per RFC
// 2397, so Kodi's parser never recognises it as a protocol and mishandles
// it entirely; confirmed live (ffmpegdirect logged "could not open file
// data:...") and via source, not assumed.
//
// A real http://127.0.0.1:<port>/... URL parses through Kodi's CURL class
// exactly like the original live Dispatcharr URL did, so this server exists
// purely to give the rewritten playlist text a URL shape Kodi can actually
// route through inputstream.ffmpegdirect. It serves only that one
// pre-computed string per recording, entirely from memory -- it never
// contacts Dispatcharr itself or proxies segment requests; ffmpeg's HLS
// demuxer fetches segments directly from Dispatcharr using the absolute
// URLs already rewritten into the playlist text, with the same
// !X-API-Key pipe-option header this addon has always relied on for that.
// Loopback-only (127.0.0.1) and OS-assigned ephemeral port: nothing here
// is meant to be reachable from outside this machine, or to collide with
// another fixed port.

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace dispatcharr
{

class LocalPlaylistServer
{
public:
  LocalPlaylistServer();
  ~LocalPlaylistServer();

  LocalPlaylistServer(const LocalPlaylistServer&) = delete;
  LocalPlaylistServer& operator=(const LocalPlaylistServer&) = delete;

  // Starts listening on 127.0.0.1 on an OS-assigned free port and spawns
  // the accept-loop thread. Returns false (with `error` set) on failure.
  // A second call while already running is a no-op that returns true.
  bool Start(std::string& error);

  // Stops the accept loop and closes the listening socket. Safe to call
  // even if Start() was never called or already failed.
  void Stop();

  // The port Start() bound to, or 0 if not currently running.
  int GetPort() const { return m_port; }

  // Registers (or replaces) the exact bytes served for
  // GET /playlist/<recordingId>.m3u8. Thread-safe -- called from whichever
  // thread handles GetRecordingStreamProperties(), read from the accept
  // thread.
  void SetPlaylist(int recordingId, const std::string& content);

private:
  void AcceptLoop();
  // Handles exactly one request on an already-accepted connection, then
  // closes it -- no keep-alive, since ffmpeg's HLS demuxer only needs to
  // fetch the manifest a handful of times at most and a fresh connection
  // per request keeps this considerably simpler.
  void HandleConnection(intptr_t clientSocket);

  std::atomic<bool> m_running{false};
  std::thread m_acceptThread;
  intptr_t m_listenSocket = -1; // native SOCKET/int, type-erased so this header doesn't need <winsock2.h>/<sys/socket.h>
  int m_port = 0;

  std::mutex m_playlistsMutex;
  std::map<int, std::string> m_playlists;
};

} // namespace dispatcharr
