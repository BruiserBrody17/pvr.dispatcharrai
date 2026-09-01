#pragma once

// Minimal RFC 6455 WebSocket client, used only to receive Dispatcharr's
// real-time recording/timer event push (see PVRDispatcharr's realtime-update
// thread) so this addon doesn't have to wait out a polling interval to
// notice a change made outside its own local actions.
//
// Built directly on libcurl's CURLOPT_CONNECT_ONLY mode rather than curl's
// own native WebSocket API (CURLOPT_WS_OPTIONS / curl_ws_recv()), which
// needs curl >= 7.86 (added October 2022) -- the prebuilt Windows curl
// dependency this addon links against (see docs/BUILDING.md) is 7.67.0,
// well short of that, and Linux/macOS builds link whatever system libcurl
// happens to be installed, which isn't guaranteed to have WebSocket support
// compiled in either. CONNECT_ONLY mode is a much older, stable feature: it
// hands over a connected socket (with TLS already handled by curl if the
// URL is https) and lets the caller speak whatever protocol it wants over
// curl_easy_send()/curl_easy_recv(), so this works on any curl new enough
// to build this addon at all.
//
// Implements just enough of RFC 6455 to open a connection, receive text
// frames (including simple fragmented-message reassembly), and answer ping
// frames -- no permessage-deflate, no client-initiated fragmentation, since
// Dispatcharr's own server needs neither for the small JSON event payloads
// this is used for, and this client never needs to send anything but a
// pong reply.

#include <cstdint>
#include <string>
#include <vector>

namespace dispatcharr
{

class WebSocketClient
{
public:
  WebSocketClient();
  ~WebSocketClient();

  WebSocketClient(const WebSocketClient&) = delete;
  WebSocketClient& operator=(const WebSocketClient&) = delete;

  // Connects and performs the RFC 6455 opening handshake against
  // ws(s)://host:port/pathAndQuery (pathAndQuery must start with '/', and
  // may include a query string, e.g. "/ws/?token=..."). Returns false
  // (with `error` set) if the TCP/TLS connect or the HTTP Upgrade
  // handshake fails.
  bool Connect(const std::string& host,
               int port,
               bool useTls,
               const std::string& pathAndQuery,
               bool verifySsl,
               int connectTimeoutSeconds,
               std::string& error);

  // Blocks until a complete text message is received, the connection is
  // closed (by either side) or fails, or `timeoutSeconds` elapses with no
  // data at all. Ping frames are answered with a pong automatically and
  // don't count as a message. Returns:
  //    1  -- `message` contains a complete text frame's payload
  //    0  -- timed out waiting for data; connection is presumed still alive
  //   -1  -- connection closed or a transport/protocol error occurred;
  //          the caller should Close() and reconnect
  int ReceiveTextMessage(std::string& message, int timeoutSeconds, std::string& error);

  void Close();
  bool IsConnected() const { return m_curl != nullptr; }

private:
  bool SendAll(const uint8_t* data, size_t len, std::string& error);
  // Reads at least one more byte into m_recvBuffer (from m_recvPos
  // onward), waiting up to timeoutSeconds for the socket to become
  // readable if no data is immediately available. Returns 1 on success,
  // 0 on timeout, -1 on error/close.
  int FillBuffer(int timeoutSeconds, std::string& error);
  // Consumes exactly `len` bytes from the buffered/received stream into
  // `out`, calling FillBuffer() as needed. Same return convention as
  // FillBuffer().
  int ReadExact(uint8_t* out, size_t len, int timeoutSeconds, std::string& error);
  bool SendPong(const std::vector<uint8_t>& payload, std::string& error);
  bool SendClose(std::string& error);

  void* m_curl = nullptr; // CURL*, kept as void* so this header doesn't need <curl/curl.h>

  std::vector<uint8_t> m_recvBuffer;
  size_t m_recvPos = 0;
};

} // namespace dispatcharr
