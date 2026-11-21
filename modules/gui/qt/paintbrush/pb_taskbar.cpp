/*****************************************************************************
 * pb_taskbar.cpp : NAI Paintbrush — Windows taskbar progress bridge
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_taskbar.hpp"

#ifdef _WIN32

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace paintbrush
{

struct TaskbarProgress::Impl
{
    Microsoft::WRL::ComPtr<ITaskbarList3> list;
    bool failed = false;

    bool ensure()
    {
        if (list)
            return true;
        if (failed)
            return false;           /* never retry a failed CoCreate forever */

        Microsoft::WRL::ComPtr<ITaskbarList3> created;
        HRESULT hr = CoCreateInstance(CLSID_TaskbarList, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(created.GetAddressOf()));
        if (FAILED(hr) || !created)
        {
            failed = true;
            return false;
        }
        if (FAILED(created->HrInit()))
        {
            failed = true;
            return false;
        }
        list = created;
        return true;
    }
};

TaskbarProgress::TaskbarProgress() : m_impl(new Impl) {}
TaskbarProgress::~TaskbarProgress() { delete m_impl; }

void TaskbarProgress::setIndeterminate(quintptr hwnd)
{
    if (!hwnd || !m_impl->ensure())
        return;
    m_impl->list->SetProgressState(reinterpret_cast<HWND>(hwnd),
                                   TBPF_INDETERMINATE);
}

void TaskbarProgress::setValue(quintptr hwnd, qint64 done, qint64 total)
{
    if (!hwnd || total <= 0 || !m_impl->ensure())
        return;
    const HWND h = reinterpret_cast<HWND>(hwnd);
    m_impl->list->SetProgressState(h, TBPF_NORMAL);
    m_impl->list->SetProgressValue(h, static_cast<ULONGLONG>(done),
                                   static_cast<ULONGLONG>(total));
}

void TaskbarProgress::clear(quintptr hwnd)
{
    if (!hwnd || !m_impl->list)
        return;                     /* nothing was ever shown */
    m_impl->list->SetProgressState(reinterpret_cast<HWND>(hwnd),
                                   TBPF_NOPROGRESS);
}

} // namespace paintbrush

#else /* !_WIN32 */

namespace paintbrush
{

struct TaskbarProgress::Impl {};

TaskbarProgress::TaskbarProgress() : m_impl(new Impl) {}
TaskbarProgress::~TaskbarProgress() { delete m_impl; }
void TaskbarProgress::setIndeterminate(quintptr) {}
void TaskbarProgress::setValue(quintptr, qint64, qint64) {}
void TaskbarProgress::clear(quintptr) {}

} // namespace paintbrush

#endif
