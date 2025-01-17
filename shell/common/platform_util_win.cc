// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/common/platform_util.h"

#include <windows.h>  // windows.h must be included first

#include <atlbase.h>
#include <comdef.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/stl_util.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/post_task.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/win/registry.h"
#include "base/win/scoped_co_mem.h"
#include "base/win/scoped_com_initializer.h"
#include "base/win/windows_version.h"
#include "content/public/browser/browser_task_traits.h"
#include "ui/base/win/shell.h"
#include "url/gurl.h"

namespace {

// Required COM implementation of IFileOperationProgressSink so we can
// precheck files before deletion to make sure they can be move to the
// Recycle Bin.
class DeleteFileProgressSink : public IFileOperationProgressSink {
 public:
  DeleteFileProgressSink();
  virtual ~DeleteFileProgressSink() = default;

 private:
  ULONG STDMETHODCALLTYPE AddRef(void) override;
  ULONG STDMETHODCALLTYPE Release(void) override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           LPVOID* ppvObj) override;
  HRESULT STDMETHODCALLTYPE StartOperations(void) override;
  HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override;
  HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override;
  HRESULT STDMETHODCALLTYPE
  PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override;
  HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD,
                                        IShellItem*,
                                        IShellItem*,
                                        LPCWSTR) override;
  HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD,
                                         IShellItem*,
                                         IShellItem*,
                                         LPCWSTR,
                                         HRESULT,
                                         IShellItem*) override;
  HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD,
                                        IShellItem*,
                                        IShellItem*,
                                        LPCWSTR) override;
  HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD,
                                         IShellItem*,
                                         IShellItem*,
                                         LPCWSTR,
                                         HRESULT,
                                         IShellItem*) override;
  HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem*) override;
  HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD,
                                           IShellItem*,
                                           HRESULT,
                                           IShellItem*) override;
  HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override;
  HRESULT STDMETHODCALLTYPE PostNewItem(DWORD,
                                        IShellItem*,
                                        LPCWSTR,
                                        LPCWSTR,
                                        DWORD,
                                        HRESULT,
                                        IShellItem*) override;
  HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override;
  HRESULT STDMETHODCALLTYPE ResetTimer(void) override;
  HRESULT STDMETHODCALLTYPE PauseTimer(void) override;
  HRESULT STDMETHODCALLTYPE ResumeTimer(void) override;

  ULONG m_cRef;
};

DeleteFileProgressSink::DeleteFileProgressSink() {
  m_cRef = 0;
}

HRESULT DeleteFileProgressSink::PreDeleteItem(DWORD dwFlags, IShellItem*) {
  if (!(dwFlags & TSF_DELETE_RECYCLE_IF_POSSIBLE)) {
    // TSF_DELETE_RECYCLE_IF_POSSIBLE will not be set for items that cannot be
    // recycled.  In this case, we abort the delete operation.  This bubbles
    // up and stops the Delete in IFileOperation.
    return E_ABORT;
  }
  // Returns S_OK if successful, or an error value otherwise. In the case of an
  // error value, the delete operation and all subsequent operations pending
  // from the call to IFileOperation are canceled.
  return S_OK;
}

HRESULT DeleteFileProgressSink::QueryInterface(REFIID riid, LPVOID* ppvObj) {
  // Always set out parameter to NULL, validating it first.
  if (!ppvObj)
    return E_INVALIDARG;
  *ppvObj = nullptr;
  if (riid == IID_IUnknown || riid == IID_IFileOperationProgressSink) {
    // Increment the reference count and return the pointer.
    *ppvObj = reinterpret_cast<IUnknown*>(this);
    AddRef();
    return NOERROR;
  }
  return E_NOINTERFACE;
}

ULONG DeleteFileProgressSink::AddRef() {
  InterlockedIncrement(&m_cRef);
  return m_cRef;
}

ULONG DeleteFileProgressSink::Release() {
  // Decrement the object's internal counter.
  ULONG ulRefCount = InterlockedDecrement(&m_cRef);
  if (0 == m_cRef) {
    delete this;
  }
  return ulRefCount;
}

HRESULT DeleteFileProgressSink::StartOperations() {
  return S_OK;
}

HRESULT DeleteFileProgressSink::FinishOperations(HRESULT) {
  return S_OK;
}

HRESULT DeleteFileProgressSink::PreRenameItem(DWORD, IShellItem*, LPCWSTR) {
  return S_OK;
}

HRESULT DeleteFileProgressSink::PostRenameItem(DWORD,
                                               IShellItem*,
                                               __RPC__in_string LPCWSTR,
                                               HRESULT,
                                               IShellItem*) {
  return E_NOTIMPL;
}

HRESULT DeleteFileProgressSink::PreMoveItem(DWORD,
                                            IShellItem*,
                                            IShellItem*,
                                            LPCWSTR) {
  return E_NOTIMPL;
}

HRESULT DeleteFileProgressSink::PostMoveItem(DWORD,
                                             IShellItem*,
                                             IShellItem*,
                                             LPCWSTR,
                                             HRESULT,
                                             IShellItem*) {
  return E_NOTIMPL;
}

HRESULT DeleteFileProgressSink::PreCopyItem(DWORD,
                                            IShellItem*,
                                            IShellItem*,
                                            LPCWSTR) {
  return E_NOTIMPL;
}

HRESULT DeleteFileProgressSink::PostCopyItem(DWORD,
                                             IShellItem*,
                                             IShellItem*,
                                             LPCWSTR,
                                             HRESULT,
                                             IShellItem*) {
  return E_NOTIMPL;
}

HRESULT DeleteFileProgressSink::PostDeleteItem(DWORD,
                                               IShellItem*,
                                               HRESULT,
                                               IShellItem*) {
  return S_OK;
}

HRESULT DeleteFileProgressSink::PreNewItem(DWORD dwFlags,
                                           IShellItem*,
                                           LPCWSTR) {
  return E_NOTIMPL;
}

HRESULT DeleteFileProgressSink::PostNewItem(DWORD,
                                            IShellItem*,
                                            LPCWSTR,
                                            LPCWSTR,
                                            DWORD,
                                            HRESULT,
                                            IShellItem*) {
  return E_NOTIMPL;
}

HRESULT DeleteFileProgressSink::UpdateProgress(UINT, UINT) {
  return S_OK;
}

HRESULT DeleteFileProgressSink::ResetTimer() {
  return S_OK;
}

HRESULT DeleteFileProgressSink::PauseTimer() {
  return S_OK;
}

HRESULT DeleteFileProgressSink::ResumeTimer() {
  return S_OK;
}

std::string OpenExternalOnWorkerThread(
    const GURL& url,
    const platform_util::OpenExternalOptions& options) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  // Quote the input scheme to be sure that the command does not have
  // parameters unexpected by the external program. This url should already
  // have been escaped.
  base::string16 escaped_url = L"\"" + base::UTF8ToUTF16(url.spec()) + L"\"";
  base::string16 working_dir = options.working_dir.value();

  if (reinterpret_cast<ULONG_PTR>(
          ShellExecuteW(nullptr, L"open", escaped_url.c_str(), nullptr,
                        working_dir.empty() ? nullptr : working_dir.c_str(),
                        SW_SHOWNORMAL)) <= 32) {
    return "Failed to open";
  }
  return "";
}

void ShowItemInFolderOnWorkerThread(const base::FilePath& full_path) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  base::win::ScopedCOMInitializer com_initializer;
  if (!com_initializer.Succeeded())
    return;

  base::FilePath dir = full_path.DirName().AsEndingWithSeparator();
  // ParseDisplayName will fail if the directory is "C:", it must be "C:\\".
  if (dir.empty())
    return;

  Microsoft::WRL::ComPtr<IShellFolder> desktop;
  HRESULT hr = SHGetDesktopFolder(desktop.GetAddressOf());
  if (FAILED(hr))
    return;

  base::win::ScopedCoMem<ITEMIDLIST> dir_item;
  hr = desktop->ParseDisplayName(NULL, NULL,
                                 const_cast<wchar_t*>(dir.value().c_str()),
                                 NULL, &dir_item, NULL);
  if (FAILED(hr)) {
    ui::win::OpenFolderViaShell(dir);
    return;
  }

  base::win::ScopedCoMem<ITEMIDLIST> file_item;
  hr = desktop->ParseDisplayName(
      NULL, NULL, const_cast<wchar_t*>(full_path.value().c_str()), NULL,
      &file_item, NULL);
  if (FAILED(hr)) {
    ui::win::OpenFolderViaShell(dir);
    return;
  }

  const ITEMIDLIST* highlight[] = {file_item};
  hr = SHOpenFolderAndSelectItems(dir_item, base::size(highlight), highlight,
                                  NULL);
  if (FAILED(hr)) {
    // On some systems, the above call mysteriously fails with "file not
    // found" even though the file is there.  In these cases, ShellExecute()
    // seems to work as a fallback (although it won't select the file).
    if (hr == ERROR_FILE_NOT_FOUND) {
      ShellExecute(NULL, L"open", dir.value().c_str(), NULL, NULL, SW_SHOW);
    } else {
      LOG(WARNING) << " " << __func__ << "(): Can't open full_path = \""
                   << full_path.value() << "\""
                   << " hr = " << logging::SystemErrorCodeToString(hr);
      ui::win::OpenFolderViaShell(dir);
    }
  }
}

}  // namespace

namespace platform_util {

void ShowItemInFolder(const base::FilePath& full_path) {
  base::CreateCOMSTATaskRunner(
      {base::ThreadPool(), base::MayBlock(), base::TaskPriority::USER_BLOCKING})
      ->PostTask(FROM_HERE,
                 base::BindOnce(&ShowItemInFolderOnWorkerThread, full_path));
}

bool OpenItem(const base::FilePath& full_path) {
  if (base::DirectoryExists(full_path))
    return ui::win::OpenFolderViaShell(full_path);
  else
    return ui::win::OpenFileViaShell(full_path);
}

void OpenExternal(const GURL& url,
                  const OpenExternalOptions& options,
                  OpenExternalCallback callback) {
  base::PostTaskAndReplyWithResult(
      base::CreateCOMSTATaskRunner({base::ThreadPool(), base::MayBlock(),
                                    base::TaskPriority::USER_BLOCKING})
          .get(),
      FROM_HERE, base::BindOnce(&OpenExternalOnWorkerThread, url, options),
      std::move(callback));
}

bool MoveItemToTrash(const base::FilePath& path, bool delete_on_fail) {
  base::win::ScopedCOMInitializer com_initializer;
  if (!com_initializer.Succeeded())
    return false;

  Microsoft::WRL::ComPtr<IFileOperation> pfo;
  if (FAILED(::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&pfo))))
    return false;

  // Elevation prompt enabled for UAC protected files.  This overrides the
  // SILENT, NO_UI and NOERRORUI flags.

  if (base::win::GetVersion() >= base::win::Version::WIN8) {
    // Windows 8 introduces the flag RECYCLEONDELETE and deprecates the
    // ALLOWUNDO in favor of ADDUNDORECORD.
    if (FAILED(pfo->SetOperationFlags(
            FOF_NO_UI | FOFX_ADDUNDORECORD | FOF_NOERRORUI | FOF_SILENT |
            FOFX_SHOWELEVATIONPROMPT | FOFX_RECYCLEONDELETE)))
      return false;
  } else {
    // For Windows 7 and Vista, RecycleOnDelete is the default behavior.
    if (FAILED(pfo->SetOperationFlags(FOF_NO_UI | FOF_ALLOWUNDO |
                                      FOF_NOERRORUI | FOF_SILENT |
                                      FOFX_SHOWELEVATIONPROMPT)))
      return false;
  }

  // Create an IShellItem from the supplied source path.
  Microsoft::WRL::ComPtr<IShellItem> delete_item;
  if (FAILED(SHCreateItemFromParsingName(
          path.value().c_str(), NULL,
          IID_PPV_ARGS(delete_item.GetAddressOf()))))
    return false;

  Microsoft::WRL::ComPtr<IFileOperationProgressSink> delete_sink(
      new DeleteFileProgressSink);
  if (!delete_sink)
    return false;

  // Processes the queued command DeleteItem. This will trigger
  // the DeleteFileProgressSink to check for Recycle Bin.
  return SUCCEEDED(pfo->DeleteItem(delete_item.Get(), delete_sink.Get())) &&
         SUCCEEDED(pfo->PerformOperations());
}

void Beep() {
  MessageBeep(MB_OK);
}

}  // namespace platform_util
