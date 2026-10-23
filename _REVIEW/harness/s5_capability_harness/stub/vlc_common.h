/*
 * stub vlc_common.h — minimal stand-in so the paintbrush services can be
 * compiled outside the VLC build for the Stream-5 acceptance harness.
 *
 * The services only ever pass vlc_object_t* around opaquely; everything
 * else from the real vlc_common.h is unused by them.
 */
#ifndef STUB_VLC_COMMON_H
#define STUB_VLC_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vlc_object_t vlc_object_t;

#ifdef __cplusplus
}
#endif

#endif /* STUB_VLC_COMMON_H */