#pragma once

#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

namespace dispatcharr
{

struct EpgEntry
{
  std::string channelTvgId;
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
// <channel id="..."> value, which corresponds to Channel::tvgId.
class XmlTvParser
{
public:
  static bool Parse(const std::string& xmlContent,
                     std::unordered_map<std::string, std::vector<EpgEntry>>& out,
                     std::string& error);
};

} // namespace dispatcharr
