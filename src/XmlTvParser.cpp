#include "XmlTvParser.h"

#include <pugixml.hpp>

#include <cstdlib>
#include <sstream>

#if defined(_WIN32)
#include <time.h>
#else
#include <ctime>
#endif

namespace dispatcharr
{

namespace
{

// Portable timegm(): interprets a struct tm as UTC and returns a time_t,
// without touching the process-wide TZ setting (unlike mktime()).
time_t PortableTimeGm(struct tm* tmVal)
{
#if defined(_WIN32)
  return _mkgmtime(tmVal);
#else
  return timegm(tmVal);
#endif
}

// XMLTV date format: "YYYYMMDDHHMMSS" optionally followed by " +HHMM".
time_t ParseXmlTvTime(const std::string& timeStr)
{
  if (timeStr.size() < 14)
    return 0;

  struct tm tmVal{};
  try
  {
    tmVal.tm_year = std::stoi(timeStr.substr(0, 4)) - 1900;
    tmVal.tm_mon = std::stoi(timeStr.substr(4, 2)) - 1;
    tmVal.tm_mday = std::stoi(timeStr.substr(6, 2));
    tmVal.tm_hour = std::stoi(timeStr.substr(8, 2));
    tmVal.tm_min = std::stoi(timeStr.substr(10, 2));
    tmVal.tm_sec = std::stoi(timeStr.substr(12, 2));
  }
  catch (const std::exception&)
  {
    return 0;
  }

  time_t utcTime = PortableTimeGm(&tmVal);

  size_t spacePos = timeStr.find(' ');
  if (spacePos != std::string::npos && spacePos + 5 < timeStr.size())
  {
    std::string offsetStr = timeStr.substr(spacePos + 1);
    if (offsetStr.size() >= 5 && (offsetStr[0] == '+' || offsetStr[0] == '-'))
    {
      try
      {
        int sign = (offsetStr[0] == '-') ? -1 : 1;
        int offsetHours = std::stoi(offsetStr.substr(1, 2));
        int offsetMinutes = std::stoi(offsetStr.substr(3, 2));
        utcTime -= sign * (offsetHours * 3600 + offsetMinutes * 60);
      }
      catch (const std::exception&)
      {
        // Ignore a malformed offset; fall back to the unadjusted UTC time.
      }
    }
  }
  return utcTime;
}

// xmltv_ns episode-num format: "season.episode.part" or "season.episode",
// each component 0-indexed and optional (e.g. "2.4." or ".4." or "2..").
void ParseEpisodeNum(const std::string& value, int& season, int& episode)
{
  season = -1;
  episode = -1;
  std::string trimmed = value;
  size_t slashPos = trimmed.find('/');
  if (slashPos != std::string::npos)
    trimmed = trimmed.substr(0, slashPos); // drop "/PP" part-of-multipart suffix

  std::stringstream ss(trimmed);
  std::string field;
  int index = 0;
  while (std::getline(ss, field, '.') && index < 2)
  {
    if (!field.empty())
    {
      try
      {
        int parsed = std::stoi(field);
        if (index == 0)
          season = parsed + 1; // xmltv_ns is 0-indexed
        else
          episode = parsed + 1;
      }
      catch (const std::exception&)
      {
        // leave as unknown
      }
    }
    ++index;
  }
}

} // namespace

bool XmlTvParser::Parse(const std::string& xmlContent,
                         std::unordered_map<std::string, std::vector<EpgEntry>>& out,
                         std::string& error)
{
  pugi::xml_document doc;
  pugi::xml_parse_result result = doc.load_buffer(xmlContent.data(), xmlContent.size());
  if (!result)
  {
    error = std::string("Failed to parse XMLTV document: ") + result.description();
    return false;
  }

  pugi::xml_node tv = doc.child("tv");
  if (!tv)
  {
    error = "XMLTV document has no <tv> root element";
    return false;
  }

  out.clear();
  for (pugi::xml_node programme : tv.children("programme"))
  {
    EpgEntry entry;
    entry.channelXmltvId = programme.attribute("channel").as_string();
    if (entry.channelXmltvId.empty())
      continue;

    entry.startTime = ParseXmlTvTime(programme.attribute("start").as_string());
    entry.endTime = ParseXmlTvTime(programme.attribute("stop").as_string());
    if (entry.startTime == 0 || entry.endTime == 0)
      continue;

    entry.title = programme.child("title").text().as_string();
    entry.subtitle = programme.child("sub-title").text().as_string();
    entry.description = programme.child("desc").text().as_string();
    entry.genre = programme.child("category").text().as_string();

    for (pugi::xml_node episodeNum : programme.children("episode-num"))
    {
      if (std::string(episodeNum.attribute("system").as_string()) == "xmltv_ns")
      {
        ParseEpisodeNum(episodeNum.text().as_string(), entry.seasonNumber, entry.episodeNumber);
        break;
      }
    }

    out[entry.channelXmltvId].push_back(std::move(entry));
  }

  return true;
}

} // namespace dispatcharr
