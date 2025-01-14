// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/browser.h"

#include <memory>
#include <string>
#include <utility>

#include "base/files/file_util.h"
#include "base/message_loop/message_loop.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "base/threading/thread_restrictions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "shell/browser/atom_browser_main_parts.h"
#include "shell/browser/atom_paths.h"
#include "shell/browser/browser_observer.h"
#include "shell/browser/login_handler.h"
#include "shell/browser/native_window.h"
#include "shell/browser/window_list.h"
#include "shell/common/application_info.h"
#include "shell/common/gin_helper/arguments.h"

namespace electron {

Browser::LoginItemSettings::LoginItemSettings() = default;
Browser::LoginItemSettings::~LoginItemSettings() = default;
Browser::LoginItemSettings::LoginItemSettings(const LoginItemSettings& other) =
    default;

Browser::Browser() {
  WindowList::AddObserver(this);
}

Browser::~Browser() {
  WindowList::RemoveObserver(this);
}

// static
Browser* Browser::Get() {
  return AtomBrowserMainParts::Get()->browser();
}

void Browser::Quit() {
  if (is_quiting_)
    return;

  is_quiting_ = HandleBeforeQuit();
  if (!is_quiting_)
    return;

  if (electron::WindowList::IsEmpty())
    NotifyAndShutdown();
  else
    electron::WindowList::CloseAllWindows();
}

void Browser::Exit(gin_helper::Arguments* args) {
  int code = 0;
  args->GetNext(&code);

  if (!AtomBrowserMainParts::Get()->SetExitCode(code)) {
    // Message loop is not ready, quit directly.
    exit(code);
  } else {
    // Prepare to quit when all windows have been closed.
    is_quiting_ = true;

    // Remember this caller so that we don't emit unrelated events.
    is_exiting_ = true;

    // Must destroy windows before quitting, otherwise bad things can happen.
    if (electron::WindowList::IsEmpty()) {
      Shutdown();
    } else {
      // Unlike Quit(), we do not ask to close window, but destroy the window
      // without asking.
      electron::WindowList::DestroyAllWindows();
    }
  }
}

void Browser::Shutdown() {
  if (is_shutdown_)
    return;

  is_shutdown_ = true;
  is_quiting_ = true;

  for (BrowserObserver& observer : observers_)
    observer.OnQuit();

  if (quit_main_message_loop_) {
    std::move(quit_main_message_loop_).Run();
  } else {
    // There is no message loop available so we are in early stage, wait until
    // the quit_main_message_loop_ is available.
    // Exiting now would leave defunct processes behind.
  }
}

std::string Browser::GetVersion() const {
  std::string ret = GetOverriddenApplicationVersion();
  if (ret.empty())
    ret = GetExecutableFileVersion();
  return ret;
}

void Browser::SetVersion(const std::string& version) {
  OverrideApplicationVersion(version);
}

std::string Browser::GetName() const {
  std::string ret = GetOverriddenApplicationName();
  if (ret.empty())
    ret = GetExecutableFileProductName();
  return ret;
}

void Browser::SetName(const std::string& name) {
  OverrideApplicationName(name);
}

int Browser::GetBadgeCount() {
  return badge_count_;
}

bool Browser::OpenFile(const std::string& file_path) {
  bool prevent_default = false;
  for (BrowserObserver& observer : observers_)
    observer.OnOpenFile(&prevent_default, file_path);

  return prevent_default;
}

void Browser::OpenURL(const std::string& url) {
  for (BrowserObserver& observer : observers_)
    observer.OnOpenURL(url);
}

void Browser::Activate(bool has_visible_windows) {
  for (BrowserObserver& observer : observers_)
    observer.OnActivate(has_visible_windows);
}

void Browser::WillFinishLaunching() {
  for (BrowserObserver& observer : observers_)
    observer.OnWillFinishLaunching();
}

void Browser::DidFinishLaunching(base::DictionaryValue launch_info) {
  // Make sure the userData directory is created.
  base::ThreadRestrictions::ScopedAllowIO allow_io;
  base::FilePath user_data;
  if (base::PathService::Get(DIR_USER_DATA, &user_data))
    base::CreateDirectoryAndGetError(user_data, nullptr);

  is_ready_ = true;
  if (ready_promise_) {
    ready_promise_->Resolve();
  }
  for (BrowserObserver& observer : observers_)
    observer.OnFinishLaunching(launch_info);
}

v8::Local<v8::Value> Browser::WhenReady(v8::Isolate* isolate) {
  if (!ready_promise_) {
    ready_promise_ = std::make_unique<gin_helper::Promise<void>>(isolate);
    if (is_ready()) {
      ready_promise_->Resolve();
    }
  }
  return ready_promise_->GetHandle();
}

void Browser::OnAccessibilitySupportChanged() {
  for (BrowserObserver& observer : observers_)
    observer.OnAccessibilitySupportChanged();
}

void Browser::RequestLogin(
    scoped_refptr<LoginHandler> login_handler,
    std::unique_ptr<base::DictionaryValue> request_details) {
  for (BrowserObserver& observer : observers_)
    observer.OnLogin(login_handler, *(request_details.get()));
}

void Browser::PreMainMessageLoopRun() {
  for (BrowserObserver& observer : observers_) {
    observer.OnPreMainMessageLoopRun();
  }
}

void Browser::SetMainMessageLoopQuitClosure(base::OnceClosure quit_closure) {
  if (is_shutdown_)
    std::move(quit_closure).Run();
  else
    quit_main_message_loop_ = std::move(quit_closure);
}

void Browser::NotifyAndShutdown() {
  if (is_shutdown_)
    return;

  bool prevent_default = false;
  for (BrowserObserver& observer : observers_)
    observer.OnWillQuit(&prevent_default);

  if (prevent_default) {
    is_quiting_ = false;
    return;
  }

  Shutdown();
}

bool Browser::HandleBeforeQuit() {
  bool prevent_default = false;
  for (BrowserObserver& observer : observers_)
    observer.OnBeforeQuit(&prevent_default);

  return !prevent_default;
}

void Browser::OnWindowCloseCancelled(NativeWindow* window) {
  if (is_quiting_)
    // Once a beforeunload handler has prevented the closing, we think the quit
    // is cancelled too.
    is_quiting_ = false;
}

void Browser::OnWindowAllClosed() {
  if (is_exiting_) {
    Shutdown();
  } else if (is_quiting_) {
    NotifyAndShutdown();
  } else {
    for (BrowserObserver& observer : observers_)
      observer.OnWindowAllClosed();
  }
}

#if defined(OS_MACOSX)
void Browser::NewWindowForTab() {
  for (BrowserObserver& observer : observers_)
    observer.OnNewWindowForTab();
}
#endif

}  // namespace electron
