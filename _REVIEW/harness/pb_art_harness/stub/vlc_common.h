/*
 * stub vlc_common.h — stand-in so the Paintbrush 2.0 art runtime, document
 * core and pb_common.cpp compile and link outside the VLC build for the art /
 * harvest / boost acceptance harnesses.
 *
 * Only the symbols the paintbrush sources actually touch are provided:
 * vlc_object_t (opaque), the VLC_FORMAT attribute, VLC_OBJECT, the message
 * priorities, var_Inherit*() and vlc_object_vaLog(). The functions are
 * implemented in ../stub_vlc.cpp with harmless defaults.
 */
#ifndef STUB_VLC_COMMON_H
#define STUB_VLC_COMMON_H

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vlc_object_t vlc_object_t;
typedef union { int64_t i_int; bool b_bool; float f_float; char *psz_string; void *p_address; } vlc_value_t;

#define VLC_FORMAT(a, b)
#define VLC_OBJECT(x) ((vlc_object_t *)(x))

enum vlc_log_type
{
    VLC_MSG_INFO = 0,
    VLC_MSG_ERR,
    VLC_MSG_WARN,
    VLC_MSG_DBG,
};

char *var_InheritString(vlc_object_t *obj, const char *name);
bool var_InheritBool(vlc_object_t *obj, const char *name);
int64_t var_InheritInteger(vlc_object_t *obj, const char *name);
void vlc_object_vaLog(vlc_object_t *obj, int prio, const char *module,
                      const char *file, unsigned line, const char *func,
                      const char *format, va_list ap);

#ifdef __cplusplus
}
#endif

#endif /* STUB_VLC_COMMON_H */
