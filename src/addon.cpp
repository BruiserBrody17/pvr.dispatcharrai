#include "PVRDispatcharr.h"

#include <kodi/AddonBase.h>

#include <mutex>

class CAddonDispatcharr : public kodi::addon::CAddonBase
{
public:
  CAddonDispatcharr() = default;

  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance,
                              KODI_ADDON_INSTANCE_HDL& hdl) override
  {
    if (instance.IsType(ADDON_INSTANCE_PVR))
    {
      kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharrai: creating PVR client instance");
      auto* pvr = new PVRDispatcharr(instance);
      {
        std::lock_guard<std::mutex> lock(m_instanceMutex);
        m_instance = pvr;
      }
      hdl = pvr;
      return ADDON_STATUS_OK;
    }
    return ADDON_STATUS_UNKNOWN;
  }

  // This addon only ever creates one PVR instance in practice (Kodi's own
  // PVR manager doesn't ask for more than one per addon), so unconditional
  // clearing is correct rather than comparing hdl against the tracked
  // pointer -- if that ever changed, this would need to become a map.
  void DestroyInstance(const kodi::addon::IInstanceInfo& instance,
                       const KODI_ADDON_INSTANCE_HDL hdl) override
  {
    std::lock_guard<std::mutex> lock(m_instanceMutex);
    m_instance = nullptr;
  }

  // Kodi calls this once per changed setting, as the *last* call in a
  // batch naming the final entry in settings.xml (per its own doc
  // comment) -- fired whenever the user edits addon settings via the GUI,
  // without restarting Kodi. Forwarded to the live PVR instance so
  // settings actually take effect immediately rather than only on the
  // next full restart. Confirmed as a real bug otherwise, not just a
  // theoretical gap: before this existed, live_timeshift_mode changed via
  // the GUI had no effect on an already-running instance until Kodi was
  // fully restarted -- the addon kept trying the now-stale mode (e.g.
  // still attempting server-side/admin-only playback right after
  // switching to Off) until then, silently defeating the setting change.
  ADDON_STATUS SetSetting(const std::string& settingName,
                          const kodi::addon::CSettingValue& settingValue) override
  {
    std::lock_guard<std::mutex> lock(m_instanceMutex);
    if (m_instance)
      return m_instance->OnAddonSettingChanged(settingName, settingValue);
    // No instance yet (or already destroyed) -- nothing to apply live to;
    // the eventual/next construction reads the new value from settings
    // itself, same as it always has.
    return ADDON_STATUS_OK;
  }

private:
  std::mutex m_instanceMutex;
  PVRDispatcharr* m_instance = nullptr;
};

ADDONCREATOR(CAddonDispatcharr)
