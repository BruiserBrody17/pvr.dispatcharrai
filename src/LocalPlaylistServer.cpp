#include "LocalPlaylistServer.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstring>

namespace dispatcharr
{

namespace
{

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
void CloseNativeSocket(NativeSocket s)
{
  closesocket(s);
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
void CloseNativeSocket(NativeSocket s)
{
  close(s);
}
#endif

NativeSocket ToNative(intptr_t s)
{
  return static_cast<NativeSocket>(s);
}

// Waits up to timeoutMs for `sock` to become readable. Returns 1 if
// readable, 0 on timeout, -1 on error.
int WaitReadable(NativeSocket sock, int timeoutMs)
{
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(sock, &readSet);
  struct timeval tv;
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
  int result = select(0, &readSet, nullptr, nullptr, &tv);
#else
  int result = select(static_cast<int>(sock) + 1, &readSet, nullptr, nullptr, &tv);
#endif
  if (result < 0)
    return -1;
  if (result == 0)
    return 0;
  return 1;
}

// Parses "GET /playlist/<digits>.m3u8 HTTP/1.1" out of a request's first
// line. Returns the recording id, or -1 if the line doesn't match.
int ParseRecordingIdFromRequestLine(const std::string& requestLine)
{
  const std::string prefix = "GET /playlist/";
  if (requestLine.compare(0, prefix.size(), prefix) != 0)
    return -1;
  size_t idStart = prefix.size();
  size_t idEnd = requestLine.find(".m3u8", idStart);
  if (idEnd == std::string::npos || idEnd == idStart)
    return -1;
  std::string idStr = requestLine.substr(idStart, idEnd - idStart);
  for (char c : idStr)
  {
    if (c < '0' || c > '9')
      return -1;
  }
  try
  {
    return std::stoi(idStr);
  }
  catch (const std::exception&)
  {
    return -1;
  }
}

} // namespace

LocalPlaylistServer::LocalPlaylistServer() = default;

LocalPlaylistServer::~LocalPlaylistServer()
{
  Stop();
}

bool LocalPlaylistServer::Start(std::string& error)
{
  if (m_running)
    return true;

#ifdef _WIN32
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
  {
    error = "WSAStartup failed";
    return false;
  }
#endif

  NativeSocket sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock == kInvalidSocket)
  {
    error = "Failed to create listening socket";
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }

  int reuse = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only, never exposed beyond this machine
  addr.sin_port = 0;                             // let the OS pick a free port

  if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0)
  {
    error = "Failed to bind loopback listening socket";
    CloseNativeSocket(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }

  if (listen(sock, 4) != 0)
  {
    error = "Failed to listen on loopback socket";
    CloseNativeSocket(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }

  struct sockaddr_in boundAddr;
  socklen_t boundAddrLen = sizeof(boundAddr);
  if (getsockname(sock, reinterpret_cast<struct sockaddr*>(&boundAddr), &boundAddrLen) != 0)
  {
    error = "Failed to read bound port";
    CloseNativeSocket(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }

  m_port = ntohs(boundAddr.sin_port);
  m_listenSocket = static_cast<intptr_t>(sock);
  m_running = true;
  m_acceptThread = std::thread(&LocalPlaylistServer::AcceptLoop, this);
  return true;
}

void LocalPlaylistServer::Stop()
{
  if (!m_running)
    return;

  m_running = false;
  if (m_listenSocket != -1)
  {
    CloseNativeSocket(ToNative(m_listenSocket));
    m_listenSocket = -1;
  }
  if (m_acceptThread.joinable())
    m_acceptThread.join();

#ifdef _WIN32
  WSACleanup();
#endif

  m_port = 0;
}

void LocalPlaylistServer::SetPlaylistProvider(int recordingId, PlaylistProvider provider)
{
  std::lock_guard<std::mutex> lock(m_providersMutex);
  m_providers[recordingId] = std::move(provider);
  // Reset first-request tracking: a fresh call to
  // GetRecordingStreamProperties() (a new playback attempt) should see its
  // own first request truncated, even if this recording id was played
  // before in this addon's lifetime.
  m_servedOnce.erase(recordingId);
}

void LocalPlaylistServer::AcceptLoop()
{
  NativeSocket listenSock = ToNative(m_listenSocket);
  while (m_running)
  {
    int waitResult = WaitReadable(listenSock, 500);
    if (waitResult <= 0)
      continue; // timeout (check m_running again) or error (listen socket likely closing)

    NativeSocket clientSock = accept(listenSock, nullptr, nullptr);
    if (clientSock == kInvalidSocket)
      continue;

    HandleConnection(static_cast<intptr_t>(clientSock));
  }
}

void LocalPlaylistServer::HandleConnection(intptr_t clientSocketHandle)
{
  NativeSocket clientSock = ToNative(clientSocketHandle);

  std::string request;
  char buffer[2048];
  // Read until we have a full header block or give up -- ffmpeg's request
  // line plus headers comfortably fits in a few KB; this is a purely local
  // loopback peer, not something that needs hardening against a hostile,
  // slow-drip sender.
  while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384)
  {
    if (WaitReadable(clientSock, 2000) <= 0)
      break;
#ifdef _WIN32
    int received = recv(clientSock, buffer, sizeof(buffer), 0);
#else
    ssize_t received = recv(clientSock, buffer, sizeof(buffer), 0);
#endif
    if (received <= 0)
      break;
    request.append(buffer, static_cast<size_t>(received));
  }

  std::string response;
  size_t lineEnd = request.find("\r\n");
  std::string requestLine = (lineEnd == std::string::npos) ? request : request.substr(0, lineEnd);
  int recordingId = ParseRecordingIdFromRequestLine(requestLine);

  std::string content;
  bool found = false;
  if (recordingId >= 0)
  {
    PlaylistProvider provider;
    bool isFirstRequest = false;
    {
      std::lock_guard<std::mutex> lock(m_providersMutex);
      auto it = m_providers.find(recordingId);
      if (it != m_providers.end())
      {
        provider = it->second;
        isFirstRequest = m_servedOnce.find(recordingId) == m_servedOnce.end();
      }
    }

    // Invoked outside the lock: this calls back into DispatcharrClient,
    // which performs real network requests to Dispatcharr -- holding
    // m_providersMutex for that would block SetPlaylistProvider() (and any
    // other request on a different recording id, though this server only
    // ever handles one connection at a time) for however long that takes.
    if (provider)
      content = provider(isFirstRequest);
    found = !content.empty();

    // Only counts as "served" once a fetch actually succeeds -- a
    // transient failure on the true first request (network hiccup, a 401
    // that couldn't self-heal in time) shouldn't cost this recording its
    // one truncated, join-forcing response on the retry that follows it.
    if (found && isFirstRequest)
    {
      std::lock_guard<std::mutex> lock(m_providersMutex);
      m_servedOnce.insert(recordingId);
    }
  }

  if (found)
  {
    response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: application/vnd.apple.mpegurl\r\n";
    response += "Content-Length: " + std::to_string(content.size()) + "\r\n";
    response += "Connection: close\r\n\r\n";
    response += content;
  }
  else
  {
    const std::string body = "Not found";
    response = "HTTP/1.1 404 Not Found\r\n";
    response += "Content-Type: text/plain\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Connection: close\r\n\r\n";
    response += body;
  }

  size_t totalSent = 0;
  while (totalSent < response.size())
  {
#ifdef _WIN32
    int sent = send(clientSock, response.data() + totalSent,
                     static_cast<int>(response.size() - totalSent), 0);
#else
    ssize_t sent = send(clientSock, response.data() + totalSent, response.size() - totalSent, 0);
#endif
    if (sent <= 0)
      break;
    totalSent += static_cast<size_t>(sent);
  }

  CloseNativeSocket(clientSock);
}

} // namespace dispatcharr
