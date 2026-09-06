#include "PVRDispatcharr.h"

#include <kodi/AddonBase.h>

#include <algorithm>
#include <mutex>
#include <vector>

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
        std::lock_guard<std::mutex> lock(m_instancesMutex);
        m_instances.push_back(pvr);
      }
      hdl = pvr;
      return ADDON_STATUS_OK;
    }
    return ADDON_STATUS_UNKNOWN;
  }

  // Kodi's own PVR manager doesn't ask for more than one instance per addon
  // in practice, so m_instances is a vector of at most one element almost
  // always -- but tracked as a real collection (erase the one matching
  // hdl, not an unconditional clear) rather than a single pointer, so that
  // if a second instance ever *is* requested (Kodi's PVR API doesn't
  // forbid it, and other PVR addons have hit concurrent-instance requests
  // for things like recording-thumbnail generation), destroying one
  // instance can't wipe the tracking for a still-alive other one.
  void DestroyInstance(const kodi::addon::IInstanceInfo& instance,
                       const KODI_ADDON_INSTANCE_HDL hdl) override
  {
    std::lock_guard<std::mutex> lock(m_instancesMutex);
    auto* pvr = static_cast<PVRDispatcharr*>(hdl);
    m_instances.erase(std::remove(m_instances.begin(), m_instances.end(), pvr), m_instances.end());
  }

  // Kodi calls this once per changed setting, as the *last* call in a
  // batch naming the final entry in settings.xml (per its own doc
  // comment) -- fired whenever the user edits addon settings via the GUI,
  // without restarting Kodi. Forwarded to every live PVR instance so
  // settings actually take effect immediately rather than only on the
  // next full restart. Confirmed as a real bug otherwise, not just a
  // theoretical gap: before this existed, live_timeshift_mode changed via
  // the GUI had no effect on an already-running instance until Kodi was
  // fully restarted -- the addon kept trying the now-stale mode (e.g.
  // still attempting server-side/admin-only playback right after
  // switching to Off) until then, silently defeating the setting change.
  // This call itself carries no per-instance identity (settings.xml is one
  // shared config for the whole addon, not per-instance) -- broadcasting
  // to every tracked instance is the only correct behavior if more than
  // one ever exists, not a "pick one" choice.
  ADDON_STATUS SetSetting(const std::string& settingName,
                          const kodi::addon::CSettingValue& settingValue) override
  {
    std::lock_guard<std::mutex> lock(m_instancesMutex);
    // No instance yet (or already destroyed): nothing to apply live to --
    // the eventual/next construction reads the new value from settings
    // itself, same as it always has.
    ADDON_STATUS status = ADDON_STATUS_OK;
    for (auto* pvr : m_instances)
    {
      ADDON_STATUS thisStatus = pvr->OnAddonSettingChanged(settingName, settingValue);
      if (thisStatus != ADDON_STATUS_OK)
        status = thisStatus;
    }
    return status;
  }

private:
  std::mutex m_instancesMutex;
  std::vector<PVRDispatcharr*> m_instances;
};

ADDONCREATOR(CAddonDispatcharr)
