/*****************************************************************************
 * pb_test_hook_gate.h : NAI Paintbrush — compile-time test-hook gate
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_TEST_HOOK_GATE_H
#define PB_TEST_HOOK_GATE_H

/* The real-process acceptance hook must never ship in a public release build.
 *
 * PAINTBRUSH_TEST_HOOK is set by the Meson option `paintbrush_test_hook`
 * (type feature, default auto: compiled in for debug/debugoptimized builds,
 * compiled out for release/minsize/custom). NDEBUG is an independent veto:
 * even if a release build defines -DPAINTBRUSH_TEST_HOOK, the hook is compiled
 * out.
 *
 * The PB_TEST_HOOK environment variable only decides whether an already
 * compiled-in hook starts listening. In a release build the class, the TCP
 * server, the controller hook-up and the test-only Q_INVOKABLE bodies do not
 * exist, so no environment variable can enable them.
 *
 * Build note: paintbrush/docs/LAMA_ONNX.md ("Test hook build gate"). */
#if defined(PAINTBRUSH_TEST_HOOK) && !defined(NDEBUG)
# define PAINTBRUSH_HAS_TEST_HOOK 1
#else
# define PAINTBRUSH_HAS_TEST_HOOK 0
#endif

#endif /* PB_TEST_HOOK_GATE_H */
