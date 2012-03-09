var GamepadService = (function() {
  var Cc = SpecialPowers.wrap(Components).classes;
  var Ci = SpecialPowers.wrap(Components).interfaces;
  return Cc["@mozilla.org/gamepad-test;1"].getService(Ci.nsIDOMGamepadServiceTest);
})();
