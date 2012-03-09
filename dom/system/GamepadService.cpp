/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Gamepad API.
 *
 * The Initial Developer of the Original Code is
 * The Mozilla Foundation.
 * Portions created by the Initial Developer are Copyright (C) 2011
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *  Ted Mielczarek <ted.mielczarek@gmail.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "mozilla/Hal.h"

#include "GamepadService.h"
#include "nsAutoPtr.h"
#include "nsFocusManager.h"
#include "nsIDOMEvent.h"
#include "nsIDOMDocument.h"
#include "nsIDOMEventTarget.h"
#include "nsDOMGamepad.h"
#include "nsIDOMGamepadButtonEvent.h"
#include "nsIDOMGamepadAxisMoveEvent.h"
#include "nsIDOMGamepadConnectionEvent.h"
#include "nsIDOMWindow.h"
#include "nsIObserver.h"
#include "nsIObserverService.h"
#include "nsIPrivateDOMEvent.h"
#include "nsIServiceManager.h"
#include "nsITimer.h"
#include "nsThreadUtils.h"
#include "mozilla/Services.h"

#include <cstddef>

namespace mozilla {
namespace dom {

// Amount of time to wait before cleaning up gamepad resources
// when no pages are listening for events.
static const int kCleanupDelayMS = 2000;
static const nsTArray<nsRefPtr<nsGlobalWindow> >::index_type NoIndex =
    nsTArray<nsRefPtr<nsGlobalWindow> >::NoIndex;

namespace {

class DestroyGamepadServiceEvent : public nsRunnable {
public:
  DestroyGamepadServiceEvent() {}

  NS_IMETHOD Run() {
    GamepadService::DestroyService();
    return NS_OK;
  }
};

class ShutdownObserver : public nsIObserver {
public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER

  ShutdownObserver() {
    nsCOMPtr<nsIObserverService> observerService = 
      mozilla::services::GetObserverService();
    observerService->AddObserver(this,
                                 NS_XPCOM_WILL_SHUTDOWN_OBSERVER_ID,
                                 false);
  }
};

NS_IMPL_ISUPPORTS1(ShutdownObserver, nsIObserver)

NS_IMETHODIMP
ShutdownObserver::Observe(nsISupports* aSubject,
                          const char* aTopic,
                          const PRUnichar* aData) {
  // Shutdown the service.
  GamepadService::GetService()->BeginShutdown();

  // Unregister while we're here.
  nsCOMPtr<nsIObserverService> observerService = 
    mozilla::services::GetObserverService();
  observerService->RemoveObserver(this, NS_XPCOM_WILL_SHUTDOWN_OBSERVER_ID);

  // And delete it soon.
  nsRefPtr<DestroyGamepadServiceEvent> event =
    new DestroyGamepadServiceEvent();
  NS_DispatchToMainThread(event, NS_DISPATCH_NORMAL);
  return NS_OK;
}

} // namespace

GamepadService* GamepadService::sSingleton = NULL;
bool GamepadService::sShutdown = false;

GamepadService::GamepadService()
  : mStarted(false),
    mShuttingDown(false),
    mFocusManager(do_GetService(FOCUSMANAGER_CONTRACTID)),
    mObserver(new ShutdownObserver())
{
}

void
GamepadService::BeginShutdown() {
  mShuttingDown = true;
  mozilla::hal::StopMonitoringGamepadStatus();
  mStarted = false;
}

void
GamepadService::AddListener(nsGlobalWindow* aWindow)
{
  if (mShuttingDown) {
    return;
  }

  if (mListeners.IndexOf(aWindow) != NoIndex)
    return; // already exists

  if (!mStarted) {
    mozilla::hal::StartMonitoringGamepadStatus();
    mStarted = true;
  }

  mListeners.AppendElement(aWindow);
}

void
GamepadService::RemoveListener(nsGlobalWindow* aWindow)
{
  if (mShuttingDown) {
    // Doesn't matter at this point. It's possible we're being called
    // as a result of our own destructor here, so just bail out.
    return;
  }

  if (mListeners.IndexOf(aWindow) == NoIndex)
    return; // doesn't exist

  mListeners.RemoveElement(aWindow);

  if (mListeners.Length() == 0 && !mShuttingDown) {
    StartCleanupTimer();
  }
}

uint32_t
GamepadService::AddGamepad(const char* id,
                           uint32_t numButtons,
                           uint32_t numAxes) {
  //TODO: get initial button/axis state
  nsRefPtr<nsDOMGamepad> gamepad =
    new nsDOMGamepad(NS_ConvertUTF8toUTF16(nsDependentCString(id)),
                     0,
                     numButtons,
                     numAxes);
  int index = -1;
  for (uint32_t i = 0; i < mGamepads.Length(); i++) {
    if (!mGamepads[i]) {
      mGamepads[i] = gamepad;
      index = i;
      break;
    }
  }
  if (index == -1) {
    mGamepads.AppendElement(gamepad);
    index = mGamepads.Length() - 1;
  }

  gamepad->SetIndex(index);
  NewConnectionEvent(index, true);

  return index;
}

void
GamepadService::RemoveGamepad(uint32_t index) {
  if (index < mGamepads.Length()) {
    mGamepads[index]->SetConnected(false);
    NewConnectionEvent(index, false);
    // If this is the last entry in the list, just remove it.
    if (index == mGamepads.Length() - 1) {
      mGamepads.RemoveElementAt(index);
    } else {
      // Otherwise just null it out and leave it, so the
      // indices of the following entries remain valid.
      mGamepads[index] = NULL;
    }
  }
}

void
GamepadService::NewButtonEvent(uint32_t index, uint32_t button, bool pressed) {
  if (mShuttingDown || index >= mGamepads.Length()) {
    return;
  }

  mGamepads[index]->SetButton(button, pressed ? 1 : 0);

  for (uint32_t i = mListeners.Length(); i > 0 ; ) {
    --i;

    // Only send events to non-background windows
    if (!mListeners[i]->GetOuterWindow() ||
        mListeners[i]->GetOuterWindow()->IsBackground())
      continue;

    if (!WindowHasSeenGamepad(mListeners[i], index)) {
      SetWindowHasSeenGamepad(mListeners[i], index);
      // This window hasn't seen this gamepad before, so
      // send a connection event first.
      NewConnectionEvent(index, true);
    }

    nsRefPtr<nsDOMGamepad> gamepad = mListeners[i]->GetGamepad(index);
    nsCOMPtr<nsIDOMDocument> domdoc;
    mListeners[i]->GetDocument(getter_AddRefs(domdoc));

    if (domdoc && gamepad) {
      gamepad->SetButton(button, pressed ? 1 : 0);
      // Fire event
      FireButtonEvent(domdoc, mListeners[i], gamepad, button, pressed);
    }
  }
}

void
GamepadService::FireButtonEvent(nsIDOMDocument *domdoc,
                                nsIDOMEventTarget *target,
                                nsDOMGamepad* gamepad,
                                uint32_t button,
                                bool pressed)
{
  nsCOMPtr<nsIDOMEvent> event;
  bool defaultActionEnabled = true;
  domdoc->CreateEvent(NS_LITERAL_STRING("MozGamepadButtonEvent"),
                      getter_AddRefs(event));
  if (!event) {
    return;
  }

  nsCOMPtr<nsIDOMGamepadButtonEvent> je = do_QueryInterface(event);

  if (!je) {
    return;
  }

  nsString name = pressed ? NS_LITERAL_STRING("MozGamepadButtonDown") :
                            NS_LITERAL_STRING("MozGamepadButtonUp");
  je->InitGamepadButtonEvent(name, false, false, gamepad, button);

  nsCOMPtr<nsIPrivateDOMEvent> privateEvent = do_QueryInterface(event);
  if (privateEvent) {
    privateEvent->SetTrusted(true);
  }

  target->DispatchEvent(event, &defaultActionEnabled);
}

void
GamepadService::NewAxisMoveEvent(uint32_t index, uint32_t axis, float value) {
  if (mShuttingDown || index >= mGamepads.Length()) {
    return;
  }
  mGamepads[index]->SetAxis(axis, value);

  for (uint32_t i = mListeners.Length(); i > 0 ; ) {
    --i;

    // Only send events to non-background windows
    if (!mListeners[i]->GetOuterWindow() ||
        mListeners[i]->GetOuterWindow()->IsBackground())
      continue;

    if (!WindowHasSeenGamepad(mListeners[i], index)) {
      SetWindowHasSeenGamepad(mListeners[i], index);
      // This window hasn't seen this gamepad before, so
      // send a connection event first.
      NewConnectionEvent(index, true);
    }

    nsRefPtr<nsDOMGamepad> gamepad = mListeners[i]->GetGamepad(index);
    nsCOMPtr<nsIDOMDocument> domdoc;
    mListeners[i]->GetDocument(getter_AddRefs(domdoc));

    if (domdoc && gamepad) {
      gamepad->SetAxis(axis, value);
      // Fire event
      FireAxisMoveEvent(domdoc, mListeners[i], gamepad, axis, value);
    }
  }
}

void
GamepadService::FireAxisMoveEvent(nsIDOMDocument* domdoc,
                                  nsIDOMEventTarget* target,
                                  nsDOMGamepad* gamepad,
                                  uint32_t axis,
                                  float value)
{
  nsCOMPtr<nsIDOMEvent> event;
  bool defaultActionEnabled = true;
  domdoc->CreateEvent(NS_LITERAL_STRING("MozGamepadAxisMoveEvent"),
                      getter_AddRefs(event));
  if (!event) {
    return;
  }

  nsCOMPtr<nsIDOMGamepadAxisMoveEvent> je = do_QueryInterface(event);

  if (!je) {
    return;
  }

  je->InitGamepadAxisMoveEvent(NS_LITERAL_STRING("MozGamepadAxisMove"),
                                false, false, gamepad, axis, value);

  nsCOMPtr<nsIPrivateDOMEvent> privateEvent = do_QueryInterface(event);
  if (privateEvent)
    privateEvent->SetTrusted(true);

  target->DispatchEvent(event, &defaultActionEnabled);
}

void
GamepadService::NewConnectionEvent(uint32_t index, bool connected)
{
  if (mShuttingDown || index >= mGamepads.Length()) {
    return;
  }

  if (connected) {
    for (uint32_t i = mListeners.Length(); i > 0 ; ) {
      --i;

      // Only send events to non-background windows
      if (!mListeners[i]->GetOuterWindow() ||
          mListeners[i]->GetOuterWindow()->IsBackground())
        continue;

      // We don't fire a connected event here unless the window
      // has seen input from at least one device.
      if (connected && !mListeners[i]->HasSeenGamepadInput()) {
        return;
      }

      SetWindowHasSeenGamepad(mListeners[i], index);

      nsRefPtr<nsDOMGamepad> gamepad = mListeners[i]->GetGamepad(index);
      nsCOMPtr<nsIDOMDocument> domdoc;
      mListeners[i]->GetDocument(getter_AddRefs(domdoc));

      if (domdoc && gamepad) {
        // Fire event
        FireConnectionEvent(domdoc, mListeners[i], gamepad, connected);
      }
    }
  } else {
    // For disconnection events, fire one at every window that has received
    // data from this gamepad.
    for (uint32_t i = mListeners.Length(); i > 0 ; ) {
      --i;

      // Only send events to non-background windows
      if (!mListeners[i]->GetOuterWindow() ||
          mListeners[i]->GetOuterWindow()->IsBackground())
        continue;

      if (WindowHasSeenGamepad(mListeners[i], index)) {
        nsRefPtr<nsDOMGamepad> gamepad = mListeners[i]->GetGamepad(index);

        nsCOMPtr<nsIDOMDocument> domdoc;
        mListeners[i]->GetDocument(getter_AddRefs(domdoc));

        if (domdoc && gamepad) {
          gamepad->SetConnected(false);
          // Fire event
          FireConnectionEvent(domdoc,
                              mListeners[i],
                              gamepad,
                              false);
        }

        if (gamepad) {
          mListeners[i]->RemoveGamepad(index);
        }
      }
    }
  }
}

void
GamepadService::FireConnectionEvent(nsIDOMDocument *domdoc,
                                    nsIDOMEventTarget *target,
                                    nsDOMGamepad* gamepad,
                                    bool connected)
{
  nsCOMPtr<nsIDOMEvent> event;
  bool defaultActionEnabled = true;
  domdoc->CreateEvent(NS_LITERAL_STRING("MozGamepadConnectionEvent"),
                      getter_AddRefs(event));
  if (!event) {
    return;
  }

  nsCOMPtr<nsIDOMGamepadConnectionEvent> je = do_QueryInterface(event);

  if (!je) {
    return;
  }

  nsString name = connected ? NS_LITERAL_STRING("MozGamepadConnected") :
                              NS_LITERAL_STRING("MozGamepadDisconnected");
  je->InitGamepadConnectionEvent(name, false, false, gamepad);

  nsCOMPtr<nsIPrivateDOMEvent> privateEvent = do_QueryInterface(event);
  if (privateEvent) {
    privateEvent->SetTrusted(true);
  }

  target->DispatchEvent(event, &defaultActionEnabled);
}

void
GamepadService::SyncGamepadState(uint32_t index, nsDOMGamepad* gamepad)
{
  if (index > mGamepads.Length())
    return;

  gamepad->SyncState(mGamepads[index]);
}

// static
GamepadService* GamepadService::GetService() {
  NS_ASSERTION(!sShutdown, "Attempted to get GamepadService after shutdown!");
  if (sShutdown) {
    // Should crash safely in release builds.
    return NULL;
  }

  if (!sSingleton) {
    sSingleton = new GamepadService();
  }
  return sSingleton;
}

// static
void GamepadService::DestroyService() {
  delete sSingleton;
  sSingleton = NULL;
  sShutdown = true;
}

bool
GamepadService::WindowHasSeenGamepad(nsGlobalWindow* aWindow, uint32_t index)
{
  nsRefPtr<nsDOMGamepad> gamepad = aWindow->GetGamepad(index);
  return gamepad != NULL;
}

void
GamepadService::SetWindowHasSeenGamepad(nsGlobalWindow* aWindow,
                                        uint32_t index,
                                        bool hasSeen)
{
  if (mListeners.IndexOf(aWindow) == NoIndex) {
    // This window isn't even listening for gamepad events.
    return;
  }

  if (hasSeen) {
    aWindow->SetHasSeenGamepadInput(true);
    nsRefPtr<nsDOMGamepad> gamepad = mGamepads[index]->Clone();
    aWindow->AddGamepad(index, gamepad);
  } else {
    aWindow->RemoveGamepad(index);
  }
}

// static
void
GamepadService::TimeoutHandler(nsITimer *aTimer, void *aClosure)
{
  // the reason that we use self, instead of just using nsITimerCallback or nsIObserver
  // is so that subclasses are free to use timers without worry about the base classes's
  // usage.
  GamepadService* self = reinterpret_cast<GamepadService*>(aClosure);
  if (!self) {
    NS_ERROR("no self");
    return;
  }

  if (self->mShuttingDown) {
    return;
  }

  if (self->mListeners.Length() == 0) {
    mozilla::hal::StopMonitoringGamepadStatus();
    self->mStarted = false;
    if (!self->mGamepads.IsEmpty()) {
      self->mGamepads.Clear();
    }
  }
}

void
GamepadService::StartCleanupTimer()
{
  if (mTimer) {
    mTimer->Cancel();
  }

  mTimer = do_CreateInstance("@mozilla.org/timer;1");
  if (mTimer) {
    mTimer->InitWithFuncCallback(TimeoutHandler,
                                 this,
                                 kCleanupDelayMS,
                                 nsITimer::TYPE_ONE_SHOT);
  }
}

/*
 * Implementation of the test service. This is just to provide a simple binding
 * of the GamepadService to JavaScript via XPCOM so that we can write Mochitests
 * that add and remove fake gamepads, avoiding the platform-specific backends.
 */
NS_IMPL_ISUPPORTS1(GamepadServiceTest, nsIDOMGamepadServiceTest)

GamepadServiceTest* GamepadServiceTest::sSingleton = NULL;

// static
already_AddRefed<GamepadServiceTest>
GamepadServiceTest::CreateService()
{
  if (sSingleton == NULL) {
    sSingleton = new GamepadServiceTest();
  }
  nsRefPtr<GamepadServiceTest> service = sSingleton;
  return service.forget();
}

GamepadServiceTest::GamepadServiceTest()
{
  /* member initializers and constructor code */
}

GamepadServiceTest::~GamepadServiceTest()
{
  /* destructor code */
}

/* long AddGamepad (in string id, in long numButtons, in long numAxes); */
NS_IMETHODIMP GamepadServiceTest::AddGamepad(const char * id, PRInt32 numButtons, PRInt32 numAxes, PRInt32 *_retval NS_OUTPARAM)
{
  *_retval = GamepadService::GetService()->AddGamepad(id, numButtons, numAxes);
  return NS_OK;
}

/* void RemoveGamepad (in long index); */
NS_IMETHODIMP GamepadServiceTest::RemoveGamepad(PRInt32 index)
{
  GamepadService::GetService()->RemoveGamepad(index);
  return NS_OK;
}

/* void NewButtonEvent (in long index, in long button, in boolean pressed); */
NS_IMETHODIMP GamepadServiceTest::NewButtonEvent(PRInt32 index, PRInt32 button, bool pressed)
{
  GamepadService::GetService()->NewButtonEvent(index, button, pressed);
  return NS_OK;
}

/* void NewAxisMoveEvent (in long index, in long axis, in float value); */
NS_IMETHODIMP GamepadServiceTest::NewAxisMoveEvent(PRInt32 index, PRInt32 axis, float value)
{
  GamepadService::GetService()->NewAxisMoveEvent(index, axis, value);
  return NS_OK;
}

} // namespace dom
} // namespace mozilla
