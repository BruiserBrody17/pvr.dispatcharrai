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
//
// The provider is given a segment cap that starts small and grows a
// little on every request for the same recording, rather than jumping
// straight from "a handful of segments" to "the full history" on the
// second request. Confirmed live (via ffmpegdirect's own av_dump_format
// line, not just this addon's own logging of what it served) that jumping
// straight to the full history on request two is NOT safe: libavformat's
// live-edge join computation -- intended to run only once, on the very
// first segment selection -- turned out to still be getting re-applied
// across several of the rapid reloads libavformat performs while it's
// still probing/settling in right after open, each time using whatever
// segment count *that* particular response happened to have. A small
// first response correctly forced the very first application of that
// computation to land on segment 0, but a second response revealing
// hundreds of segments at once, arriving while probing was still
// ongoing, caused a later application of the same computation to land
// far from 0 instead -- confirmed by a real, reproduced failure where
// playback consistently joined at very close to the recording's current
// age (start: 118s on a ~2-minute-old recording, start: 734s on a
// ~12-minute-old one) despite every request being correctly logged as
// truncated-and-starting-from-seg_00000 on this addon's own side.
//
// A bounded per-request growth step alone turned out not to be quite
// enough, either: confirmed live a second time, with the cap growing
// immediately from the first request, that even a *second* application
// of the join computation (still within the settling window, just one
// reload later) using the grown-by-one-step count could land noticeably
// off 0 -- a reproducible, exact 12-second offset (start: 13.4s instead
// of 1.4s on the same recording), matching a live_start_index=-3 join
// computed against a 6-segment response (the cap after exactly one
// growth step from an initial 3) rather than the original 3-segment one.
// Fixed by holding the cap at its initial value for several requests
// (kHoldRequestsAtInitialCap) before starting to grow it at all, giving
// libavformat's own settling process more chances to finish while the
// cap -- and therefore the join computation's result, however many times
// it actually gets re-applied during that window -- stays fixed at
// exactly 0. Growth only begins once that hold period has passed, by
// which point re-application is no longer occurring in practice.
//
// See FetchInProgressPlaylistSnapshot()'s comment for the rest of the
// reasoning.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace dispatcharr
{

class LocalPlaylistServer
{
public:
  // Called for each GET request for a given recording id. `maxSegments` is
  // the cap this specific response should be truncated to -- starts small
  // and grows by a fixed step on each subsequent request for the same
  // recording id (see the class comment for why a small, gradual growth
  // step is used instead of jumping straight to the full history).
  // Returning an empty string results in a 404 -- used when the
  // underlying fetch itself fails, rather than serving broken/partial
  // content (and doesn't advance the growth step for next time, so a
  // transient failure on an early request doesn't cost it its turn).
  using PlaylistProvider = std::function<std::string(int maxSegments)>;

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
  // the same id resets its growth-step tracking back to the initial cap,
  // so a fresh playback session started against a recording that was
  // previously played (and whose provider is being re-registered by a new
  // call to GetRecordingStreamProperties()) correctly starts small again
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
  std::map<int, int> m_requestCount; // per-recording count of successful requests served so far
};

} // namespace dispatcharr
