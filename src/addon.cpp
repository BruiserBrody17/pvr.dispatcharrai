#include "PVRDispatcharr.h"

#include <kodi/AddonBase.h>

class CAddonDispatcharr : public kodi::addon::CAddonBase
{
public:
  CAddonDispatcharr() = default;

  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance,
                              KODI_ADDON_INSTANCE_HDL& hdl) override
  {
    if (instance.IsType(ADDON_INSTANCE_PVR))
    {
      kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharr: creating PVR client instance");
      hdl = new PVRDispatcharr(instance);
      return ADDON_STATUS_OK;
    }
    return ADDON_STATUS_UNKNOWN;
  }
};

ADDONCREATOR(CAddonDispatcharr)
