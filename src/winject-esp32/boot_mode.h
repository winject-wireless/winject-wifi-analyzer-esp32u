#ifndef WINJECT_BOOT_MODE_H_
#define WINJECT_BOOT_MODE_H_

// Boot mode persisted by `reset mode=...`. The sample server has a single
// runtime; both values boot it (OTA mode is kept for settings compatibility).
enum WinjectMode
{
    WINJECT_MODE_STANDALONE = 1,
    WINJECT_MODE_OTA = 2,
};

inline const char* boot_mode_name(WinjectMode mode)
{
    return mode == WINJECT_MODE_OTA ? "OTA" : "WINJECT";
}

#endif  // WINJECT_BOOT_MODE_H_
