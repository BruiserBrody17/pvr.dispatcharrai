#pragma once

// Shared by DispatcharrClient.cpp and XmlTvParser.cpp, which were each
// independently defining byte-identical copies of these two functions --
// pulled out here rather than having one include the other, since both are
// meant to stay self-contained w.r.t. each other (see CLAUDE.md's repo
// layout note).

#include <ctime>

namespace dispatcharr
{

// Portable timegm(): interprets a struct tm as UTC and returns a time_t,
// without touching the process-wide TZ setting (unlike mktime()).
inline time_t PortableTimeGm(struct tm* tmVal)
{
#if defined(_WIN32)
  return _mkgmtime(tmVal);
#else
  return timegm(tmVal);
#endif
}

// Portable gmtime(): fills tmValOut from t, interpreting t as UTC.
inline void GmTimeUtc(time_t t, struct tm* tmValOut)
{
#if defined(_WIN32)
  gmtime_s(tmValOut, &t);
#else
  gmtime_r(&t, tmValOut);
#endif
}

} // namespace dispatcharr
