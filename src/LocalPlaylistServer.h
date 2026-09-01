#pragma once

// A minimal, loopback-only HTTP server that exists for exactly one reason:
// PVR_STREAM_PROPERTY_STREAMURL's pipe-delimited "url|option=value" syntax
// is parsed by Kodi's own CURL class (xbmc/URL.cpp), which hard-requires
// the literal substring "://" to recognise a protocol at all
// (CURL::Parse(): `strURL.find("://")`). A standard `data:` URI -- the
// first approach tried for serving a rewritten copy of an in-progress
// recording's HLS playlist (see
// DispatcharrClient::FetchInProgressPlaylistSnapshot()'s comment for why
// that rewrite is needed at all) -- has no "://" anywhere in it per RFC
// 2397, so Kodi's parser never recognises it as a protocol and mishandles
// it entirely; confirmed live (ffmpegdirect logged "could not open file
// data:...") and via source, not assumed.
//
// A real http://127.0.0.1:<port>/... URL parses through Kodi's CURL class
// exactly like the original live Dispatcharr URL did, so this server exists
// purely to give the rewritten playlist text a URL shape Kodi can actually
// route through inputstream.ffmpegdirect. It never contacts Dispatcharr
// itself or proxies segment requests; ffmpeg's HLS demuxer fetches segments
// directly from Dispatcharr using the absolute URLs already rewritten into
// the playlist text, with the same !X-API-Key pipe-option header this
// addon has always relied on for that. Loopback-only (127.0.0.1) and
// OS-assigned ephemeral port: nothing here is meant to be reachable from
// outside this machine, or to collide with another fixed port.
//
// Each request is answered by calling back into a per-recording provider
// function rather than serving a fixed, pre-computed string: libavformat's
// HLS demuxer reloads a playlist that never claims to be #EXT-X-ENDLIST-
// terminated throughout playback on its own (see
// FetchInProgressPlaylistSnapshot()'s comment), and a fresh fetch+rewrite
// on each of those reloads is what actually lets a single playback session
// keep tailing newly-recorded segments as the underlying recording grows,
// rather than being frozen at whatever existed the moment playback opened.
// The provider also needs to know whether a given request is the very
// first one for that recording, since that first response has to be
// deliberately truncated to force libavformat's live-edge join logic to
// land on the true first segment (again, see
// FetchInProgressPlaylistSnapshot()'s comment for exactly why) --
// everything after the first request should reflect the full, untruncated
// history.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace dispatcharr
{

class LocalPlaylistServer
{
public:
  // Called for each GET request for a given recording id. `isFirstRequest`
  // is true only the very first time this provider is invoked for that id
  // (see the class comment for why that matters). Returning an empty
  // string results in a 404 -- used when the underlying fetch itself
  // fails, rather than serving broken/partial content.
  using PlaylistProvider = std::function<std::string(bool isFirstRequest)>;

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

  // Registers (or replaces) the provider used to answer
  // GET /playlist/<recordingId>.m3u8. Replacing an existing provider for
  // the same id resets its "first request" tracking, so a fresh playback
  // session started against a recording that was previously played (and
  // whose provider is being re-registered by a new call to
  // GetRecordingStreamProperties()) correctly gets the truncated response
  // on its own first request. Thread-safe -- called from whichever thread
  // handles GetRecordingStreamProperties(), read from the accept thread.
  void SetPlaylistProvider(int recordingId, PlaylistProvider provider);

private:
  void AcceptLoop();
  // Handles exactly one request on an already-accepted connection, then
  // closes it -- no keep-alive, since a fresh connection per request keeps
  // this considerably simpler and libavformat's HLS demuxer doesn't need
  // one to reload a playlist repeatedly.
  void HandleConnection(intptr_t clientSocket);

  std::atomic<bool> m_running{false};
  std::thread m_acceptThread;
  intptr_t m_listenSocket = -1; // native SOCKET/int, type-erased so this header doesn't need <winsock2.h>/<sys/socket.h>
  int m_port = 0;

  std::mutex m_providersMutex;
  std::map<int, PlaylistProvider> m_providers;
  std::set<int> m_servedOnce; // which recording ids have answered at least one request
};

} // namespace dispatcharr
