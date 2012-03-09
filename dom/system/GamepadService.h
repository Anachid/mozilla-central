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

#ifndef mozilla_dom_GamepadService_h_
#define mozilla_dom_GamepadService_h_

#include "mozilla/StdInt.h"
#include "nsAutoPtr.h"
#include "nsCOMArray.h"
#include "nsDOMGamepad.h"
#include "nsIDOMGamepadServiceTest.h"
#include "nsGlobalWindow.h"
#include "nsIFocusManager.h"
#include "nsIObserver.h"
#include "nsITimer.h"
#include "nsTArray.h"

class nsIDOMDocument;
class nsIDOMEventTarget;

namespace mozilla {
namespace dom {

class GamepadService {
 public:
  // Get the singleton service
  static GamepadService* GetService();
  // Destroy the singleton.
  static void DestroyService();

  void BeginShutdown();

  // Indicate that |aWindow| wants to receive gamepad events.
  void AddListener(nsGlobalWindow* aWindow);
  // Indicate that |aWindow| should no longer receive gamepad events.
  void RemoveListener(nsGlobalWindow* aWindow);

  // Add a gamepad to the list of known gamepads, and return its index.
  uint32_t AddGamepad(const char* id, uint32_t numButtons, uint32_t numAxes);
  // Remove the gamepad at |index| from the list of known gamepads.
  void RemoveGamepad(uint32_t index);

  void NewButtonEvent(uint32_t index, uint32_t button, bool pressed);
  void NewAxisMoveEvent(uint32_t  index, uint32_t axis, float value);

  // Synchronize the state of gamepad to match the gamepad stored at index.
  void SyncGamepadState(uint32_t index, nsDOMGamepad* gamepad);

 protected:
  GamepadService();
  ~GamepadService() {};
  void StartCleanupTimer();

  void NewConnectionEvent(uint32_t index, bool connected);
  void FireAxisMoveEvent(nsIDOMDocument* domdoc,
                         nsIDOMEventTarget* target,
                         nsDOMGamepad* gamepad,
                         uint32_t axis,
                         float value);
  void FireButtonEvent(nsIDOMDocument* domdoc,
                       nsIDOMEventTarget* target,
                       nsDOMGamepad* gamepad,
                       uint32_t button,
                       bool pressed);
  void FireConnectionEvent(nsIDOMDocument* domdoc,
                           nsIDOMEventTarget* target,
                           nsDOMGamepad* gamepad,
                           bool connected);

  // true if the platform-specific backend has started work
  bool mStarted;
  // true when shutdown has begun
  bool mShuttingDown;

 private:
  // Returns true if we have already sent data from this gamepad
  // to this window. This should only return true if the user
  // explicitly interacted with a gamepad while this window
  // was focused, by pressing buttons or similar actions.
  bool WindowHasSeenGamepad(nsGlobalWindow* window, uint32_t index);
  // Indicate that a window has recieved data from a gamepad.
  void SetWindowHasSeenGamepad(nsGlobalWindow* window, uint32_t index,
                               bool hasSeen = true);

  static void TimeoutHandler(nsITimer* aTimer, void* aClosure);
  static GamepadService* sSingleton;
  static bool sShutdown;

  // Gamepads connected to the system. Copies of these are handed out
  // to each window.
  nsTArray<nsRefPtr<nsDOMGamepad> > mGamepads;
  // nsGlobalWindows that are listening for gamepad events.
  // has been sent to that window.
  nsTArray<nsRefPtr<nsGlobalWindow> > mListeners;
  nsCOMPtr<nsITimer> mTimer;
  nsCOMPtr<nsIFocusManager> mFocusManager;
  nsCOMPtr<nsIObserver> mObserver;
  // Used for convenience when enumerating mListeners entries.
  int mDisconnectingGamepad;
};

// Service for testing purposes
class GamepadServiceTest : public nsIDOMGamepadServiceTest
{
public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIDOMGAMEPADSERVICETEST

  GamepadServiceTest();

  static already_AddRefed<GamepadServiceTest> CreateService();

private:
  static GamepadServiceTest* sSingleton;
  ~GamepadServiceTest();
};

}
}

#define NS_GAMEPAD_TEST_CID \
{ 0xfb1fcb57, 0xebab, 0x4cf4, \
{ 0x96, 0x3b, 0x1e, 0x4d, 0xb8, 0x52, 0x16, 0x96 } }
#define NS_GAMEPAD_TEST_CONTRACTID "@mozilla.org/gamepad-test;1"

#endif // mozilla_dom_GamepadService_h_
