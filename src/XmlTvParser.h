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
  std::string subtitle; // xmltv <sub-title> -- episode title, not a plot summary
  std::string description;
  std::vector<std::string> categories; // xmltv <category>, 0+ (can repeat)
  std::string iconPath;                // xmltv <icon src="..."> -- per-programme
                                        // poster/artwork, distinct from the
                                        // channel's own logo
  std::string cast;     // joined actor/presenter/guest/producer/commentator/
                         // composer/editor credits, comma-separated
                         // (EPG_STRING_TOKEN_SEPARATOR)
  std::string director; // joined <credits><director>, comma-separated
  std::string writer;   // joined <credits><writer>/<adapter>, comma-separated
  int year = 0;          // parsed from <date>'s leading YYYY; 0 = unknown
  std::string firstAired; // <date> verbatim (YYYY-MM-DD or just YYYY), empty = unknown
  bool isNew = false;      // <new/> present
  bool isPremiere = false; // <premiere/> present
  bool isLive = false;     // <live/> present
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
