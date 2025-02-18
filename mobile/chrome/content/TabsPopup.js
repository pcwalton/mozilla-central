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
 * The Original Code is Mozilla Mobile Browser.
 *
 * The Initial Developer of the Original Code is
 * Mozilla Foundation.
 * Portions created by the Initial Developer are Copyright (C) 2011
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Matt Brubeck <mbrubeck@mozilla.com>
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

var TabsPopup = {
  init: function() {
    Elements.tabs.addEventListener("TabOpen", this, true);
    Elements.tabs.addEventListener("TabRemove", this, true);

    this._updateTabsCount();
  },

  get box() {
    delete this.box;
    return this.box = document.getElementById("tabs-sidebar");
  },

  get list() {
    delete this.list;
    return this.list = document.getElementById("tabs");
  },

  get button() {
    delete this.button;
    return this.button = document.getElementById("tool-tabs");
  },

  hide: function hide() {
    this.box.removeAttribute("open");
    BrowserUI.popPopup(this);
  },

  show: function show() {
    // Set the box position.
    this.box.setAttribute("open", "true");
    this.list.resize();
    BrowserUI.pushPopup(this, [this.box, this.button]);
  },

  toggle: function toggle() {
    if (this.box.hasAttribute("open"))
      this.hide();
    else
      this.show();
  },

  _updateTabsCount: function() {
    let cmd = document.getElementById("cmd_showTabs");
    cmd.setAttribute("label", Browser.tabs.length);
  },

  handleEvent: function handleEvent(aEvent) {
    this._updateTabsCount();
  }
};
