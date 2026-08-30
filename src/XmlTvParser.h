#pragma once

#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

namespace dispatcharr
{

struct EpgEntry
{
  std::string channelXmltvId;
  std::string title;
  std::string subtitle;
  std::string description;
  std::string genre;
  time_t startTime = 0;
  time_t endTime = 0;
  int seasonNumber = -1;  // -1 = unknown; xmltv_ns is 0-indexed, we store as
  int episodeNumber = -1; // human-readable (1-indexed) once parsed
};

// Parses a Dispatcharr XMLTV guide document (as returned by
// GET {base}/output/epg) into programme entries keyed by the XMLTV
// <channel id="..."> value. Confirmed against a live instance: Dispatcharr
// uses the channel's channel_number here, NOT its tvg_id (e.g. USA Network
// with channel_number 2632 and tvg_id "USANetwork.us" appears in the XMLTV
// as <channel id="2632">) -- match on Channel::channelNumber, not tvgId.
class XmlTvParser
{
public:
  static bool Parse(const std::string& xmlContent,
                     std::unordered_map<std::string, std::vector<EpgEntry>>& out,
                     std::string& error);
};

} // namespace dispatcharr
