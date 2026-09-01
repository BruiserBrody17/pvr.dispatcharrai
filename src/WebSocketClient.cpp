#include "WebSocketClient.h"

#include <curl/curl.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

namespace dispatcharr
{

namespace
{

constexpr size_t kMaxFramePayload = 10 * 1024 * 1024; // sanity cap, not a real limit Dispatcharr hits

std::string Base64Encode(const uint8_t* data, size_t len)
{
  static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= len)
  {
    uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) |
                 static_cast<uint32_t>(data[i + 2]);
    out += table[(n >> 18) & 0x3F];
    out += table[(n >> 12) & 0x3F];
    out += table[(n >> 6) & 0x3F];
    out += table[n & 0x3F];
    i += 3;
  }
  size_t remaining = len - i;
  if (remaining == 1)
  {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    out += table[(n >> 18) & 0x3F];
    out += table[(n >> 12) & 0x3F];
    out += "==";
  }
  else if (remaining == 2)
  {
    uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
    out += table[(n >> 18) & 0x3F];
    out += table[(n >> 12) & 0x3F];
    out += table[(n >> 6) & 0x3F];
    out += "=";
  }
  return out;
}

std::string ToLower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

void RandomBytes(uint8_t* out, size_t len)
{
  static thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<int> dist(0, 255);
  for (size_t i = 0; i < len; ++i)
    out[i] = static_cast<uint8_t>(dist(rng));
}

} // namespace

WebSocketClient::WebSocketClient() = default;

WebSocketClient::~WebSocketClient()
{
  Close();
}

void WebSocketClient::Close()
{
  if (m_curl)
  {
    curl_easy_cleanup(static_cast<CURL*>(m_curl));
    m_curl = nullptr;
  }
  m_recvBuffer.clear();
  m_recvPos = 0;
}

bool WebSocketClient::SendAll(const uint8_t* data, size_t len, std::string& error)
{
  CURL* curl = static_cast<CURL*>(m_curl);
  size_t sent = 0;
  while (sent < len)
  {
    size_t n = 0;
    CURLcode res = curl_easy_send(curl, data + sent, len - sent, &n);
    if (res == CURLE_AGAIN)
    {
      curl_socket_t sockfd = CURL_SOCKET_BAD;
      curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd);
      fd_set writeFds;
      FD_ZERO(&writeFds);
      FD_SET(sockfd, &writeFds);
      struct timeval tv{};
      tv.tv_sec = 5;
#ifdef _WIN32
      select(0, nullptr, &writeFds, nullptr, &tv);
#else
      select(static_cast<int>(sockfd) + 1, nullptr, &writeFds, nullptr, &tv);
#endif
      continue;
    }
    if (res != CURLE_OK)
    {
      error = std::string("WebSocket send failed: ") + curl_easy_strerror(res);
      return false;
    }
    sent += n;
  }
  return true;
}

int WebSocketClient::FillBuffer(int timeoutSeconds, std::string& error)
{
  CURL* curl = static_cast<CURL*>(m_curl);
  curl_socket_t sockfd = CURL_SOCKET_BAD;
  curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd);

  uint8_t chunk[4096];
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
  for (;;)
  {
    size_t n = 0;
    CURLcode res = curl_easy_recv(curl, chunk, sizeof(chunk), &n);
    if (res == CURLE_OK)
    {
      if (n == 0)
      {
        error = "WebSocket connection closed by peer";
        return -1;
      }
      m_recvBuffer.insert(m_recvBuffer.end(), chunk, chunk + n);
      return 1;
    }
    if (res == CURLE_AGAIN)
    {
      auto now = std::chrono::steady_clock::now();
      if (now >= deadline)
        return 0;
      auto remainingMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
      fd_set readFds;
      FD_ZERO(&readFds);
      FD_SET(sockfd, &readFds);
      struct timeval tv{};
      tv.tv_sec = static_cast<long>(remainingMs / 1000);
      tv.tv_usec = static_cast<long>((remainingMs % 1000) * 1000);
#ifdef _WIN32
      int rc = select(0, &readFds, nullptr, nullptr, &tv);
#else
      int rc = select(static_cast<int>(sockfd) + 1, &readFds, nullptr, nullptr, &tv);
#endif
      if (rc == 0)
        return 0; // timed out with nothing to show for it
      if (rc < 0)
      {
        error = "WebSocket select() failed while waiting for data";
        return -1;
      }
      continue; // socket says readable; loop back to curl_easy_recv
    }
    error = std::string("WebSocket recv failed: ") + curl_easy_strerror(res);
    return -1;
  }
}

int WebSocketClient::ReadExact(uint8_t* out, size_t len, int timeoutSeconds, std::string& error)
{
  while (m_recvBuffer.size() - m_recvPos < len)
  {
    int r = FillBuffer(timeoutSeconds, error);
    if (r <= 0)
      return r;
  }
  std::memcpy(out, m_recvBuffer.data() + m_recvPos, len);
  m_recvPos += len;
  // Compact once consumed data dominates the buffer, so a long-lived
  // connection doesn't grow this vector forever.
  if (m_recvPos > 0 && m_recvPos * 2 > m_recvBuffer.size())
  {
    m_recvBuffer.erase(m_recvBuffer.begin(), m_recvBuffer.begin() + static_cast<long>(m_recvPos));
    m_recvPos = 0;
  }
  return 1;
}

bool WebSocketClient::Connect(const std::string& host,
                              int port,
                              bool useTls,
                              const std::string& pathAndQuery,
                              bool verifySsl,
                              int connectTimeoutSeconds,
                              std::string& error)
{
  Close();

  CURL* curl = curl_easy_init();
  if (!curl)
  {
    error = "Failed to initialise libcurl";
    return false;
  }

  std::string scheme = useTls ? "https://" : "http://";
  std::string url = scheme + host + ":" + std::to_string(port) + "/";

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verifySsl ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verifySsl ? 2L : 0L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(connectTimeoutSeconds));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(connectTimeoutSeconds));

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK)
  {
    error = std::string("WebSocket TCP/TLS connect failed: ") + curl_easy_strerror(res);
    curl_easy_cleanup(curl);
    return false;
  }

  m_curl = curl;

  uint8_t nonce[16];
  RandomBytes(nonce, sizeof(nonce));
  std::string key = Base64Encode(nonce, sizeof(nonce));

  std::string request = "GET " + pathAndQuery +
                         " HTTP/1.1\r\n"
                         "Host: " +
                         host + ":" + std::to_string(port) +
                         "\r\n"
                         "Upgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         "Sec-WebSocket-Key: " +
                         key +
                         "\r\n"
                         "Sec-WebSocket-Version: 13\r\n"
                         "\r\n";

  if (!SendAll(reinterpret_cast<const uint8_t*>(request.data()), request.size(), error))
  {
    Close();
    return false;
  }

  // Read the HTTP response until the blank line that ends the headers.
  // Anything read past that point is the start of the WebSocket frame
  // stream and stays buffered (m_recvPos) for ReceiveTextMessage().
  std::string headerText;
  for (;;)
  {
    // Search what's already buffered before pulling more off the wire.
    const char* base = reinterpret_cast<const char*>(m_recvBuffer.data()) + m_recvPos;
    size_t available = m_recvBuffer.size() - m_recvPos;
    headerText.assign(base, available);
    size_t terminator = headerText.find("\r\n\r\n");
    if (terminator != std::string::npos)
    {
      m_recvPos += terminator + 4;
      headerText.resize(terminator);
      break;
    }
    int r = FillBuffer(connectTimeoutSeconds, error);
    if (r <= 0)
    {
      if (r == 0)
        error = "Timed out waiting for the WebSocket handshake response";
      Close();
      return false;
    }
  }

  std::string headerLower = ToLower(headerText);
  bool got101 = headerLower.find(" 101 ") != std::string::npos ||
                headerLower.rfind("http/1.1 101", 0) == 0 || headerLower.rfind("http/1.0 101", 0) == 0;
  bool gotUpgrade = headerLower.find("upgrade: websocket") != std::string::npos;
  if (!got101 || !gotUpgrade)
  {
    error = "Dispatcharr did not accept the WebSocket upgrade (check the account can "
            "authenticate; response: " +
            headerText.substr(0, 200) + ")";
    Close();
    return false;
  }

  return true;
}

bool WebSocketClient::SendPong(const std::vector<uint8_t>& payload, std::string& error)
{
  uint8_t maskKey[4];
  RandomBytes(maskKey, sizeof(maskKey));

  std::vector<uint8_t> frame;
  frame.push_back(0x80 | 0x0A); // FIN + opcode PONG
  size_t len = payload.size();
  if (len <= 125)
  {
    frame.push_back(0x80 | static_cast<uint8_t>(len)); // MASK bit set
  }
  else
  {
    // Pong payloads are always tiny (an echoed ping payload, itself capped
    // at 125 bytes by the spec) -- this branch is defensive, not expected.
    frame.push_back(0x80 | 126);
    frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(len & 0xFF));
  }
  frame.insert(frame.end(), maskKey, maskKey + 4);
  for (size_t i = 0; i < len; ++i)
    frame.push_back(payload[i] ^ maskKey[i % 4]);

  return SendAll(frame.data(), frame.size(), error);
}

bool WebSocketClient::SendClose(std::string& error)
{
  uint8_t maskKey[4];
  RandomBytes(maskKey, sizeof(maskKey));
  uint8_t frame[6] = {0x80 | 0x08, 0x80, maskKey[0], maskKey[1], maskKey[2], maskKey[3]};
  return SendAll(frame, sizeof(frame), error);
}

int WebSocketClient::ReceiveTextMessage(std::string& message, int timeoutSeconds, std::string& error)
{
  message.clear();
  std::vector<uint8_t> assembled;
  bool assembling = false;

  for (;;)
  {
    uint8_t header[2];
    int r = ReadExact(header, 2, timeoutSeconds, error);
    if (r <= 0)
      return r;

    bool fin = (header[0] & 0x80) != 0;
    uint8_t opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t len = header[1] & 0x7F;

    if (len == 126)
    {
      uint8_t ext[2];
      r = ReadExact(ext, 2, timeoutSeconds, error);
      if (r <= 0)
        return r;
      len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    }
    else if (len == 127)
    {
      uint8_t ext[8];
      r = ReadExact(ext, 8, timeoutSeconds, error);
      if (r <= 0)
        return r;
      len = 0;
      for (int i = 0; i < 8; ++i)
        len = (len << 8) | ext[i];
    }
    if (len > kMaxFramePayload)
    {
      error = "WebSocket frame payload exceeds sanity limit";
      return -1;
    }

    uint8_t maskKey[4] = {0, 0, 0, 0};
    if (masked)
    {
      r = ReadExact(maskKey, 4, timeoutSeconds, error);
      if (r <= 0)
        return r;
    }

    std::vector<uint8_t> payload(static_cast<size_t>(len));
    if (len > 0)
    {
      r = ReadExact(payload.data(), payload.size(), timeoutSeconds, error);
      if (r <= 0)
        return r;
      if (masked)
      {
        for (size_t i = 0; i < payload.size(); ++i)
          payload[i] ^= maskKey[i % 4];
      }
    }

    switch (opcode)
    {
      case 0x9: // ping
        if (!SendPong(payload, error))
          return -1;
        continue;
      case 0xA: // pong
        continue;
      case 0x8: // close
        SendClose(error);
        error = "WebSocket connection closed by peer (close frame)";
        return -1;
      case 0x1: // text
      case 0x0: // continuation
        assembled.insert(assembled.end(), payload.begin(), payload.end());
        assembling = true;
        if (fin)
        {
          message.assign(reinterpret_cast<const char*>(assembled.data()), assembled.size());
          return 1;
        }
        continue;
      case 0x2: // binary -- unexpected for this server's event payloads; skip it
      default:
        if (fin && !assembling)
          continue; // a standalone, uninteresting frame -- keep listening
        continue;
    }
  }
}

} // namespace dispatcharr
