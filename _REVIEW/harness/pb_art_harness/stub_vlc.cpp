/* Harness-only implementations of the few VLC core symbols pb_common.cpp uses. */
#include "stub/vlc_common.h"

#include <cstdio>

extern "C" {

char *var_InheritString(vlc_object_t *, const char *) { return nullptr; }
bool var_InheritBool(vlc_object_t *, const char *) { return false; }
int64_t var_InheritInteger(vlc_object_t *, const char *) { return 0; }

void vlc_object_vaLog(vlc_object_t *, int prio, const char *module,
                      const char *, unsigned, const char *,
                      const char *format, va_list ap)
{
    if (prio == VLC_MSG_DBG)
        return; /* keep harness output readable */
    std::fprintf(stderr, "[%s] ", module ? module : "vlc");
    std::vfprintf(stderr, format, ap);
    std::fputc('\n', stderr);
}

} /* extern "C" */
