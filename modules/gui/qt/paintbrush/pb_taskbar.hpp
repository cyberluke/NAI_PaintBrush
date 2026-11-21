/*****************************************************************************
 * pb_taskbar.hpp : NAI Paintbrush — Windows taskbar progress bridge
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#ifndef PB_TASKBAR_HPP
#define PB_TASKBAR_HPP

#include <QtGlobal>

namespace paintbrush
{

/* Minimal ITaskbarList3 progress bridge.
 *
 * Threading: Qt 6 removed QtWinExtras, so this talks COM directly. It MUST be
 * called from the GUI thread (the one that owns the window and has COM
 * initialised) — SegmentationService emits on the worker thread, therefore the
 * controller marshals these calls with a queued invocation.
 *
 * The taskbar button may not exist yet at the first call (Windows broadcasts
 * TaskbarButtonCreated later); every method no-ops until COM accepts the value
 * and the next state update retries.
 *
 * PIMPL: the COM interface is complete only where <shobjidl.h> is included, so
 * it must not appear in this header — otherwise every TU including it fails on
 * WRL::ComPtr with an incomplete type. */
class TaskbarProgress
{
public:
    TaskbarProgress();
    ~TaskbarProgress();
    TaskbarProgress(const TaskbarProgress &) = delete;
    TaskbarProgress &operator=(const TaskbarProgress &) = delete;

    /* TBPF_INDETERMINATE while an opaque phase (model load) is running. */
    void setIndeterminate(quintptr hwnd);
    /* TBPF_NORMAL with a real fraction (job counts, encode progress). */
    void setValue(quintptr hwnd, qint64 done, qint64 total);
    /* TBPF_NOPROGRESS: back to a clean taskbar. */
    void clear(quintptr hwnd);

private:
    struct Impl;
    Impl *m_impl = nullptr;
};

} // namespace paintbrush

#endif
