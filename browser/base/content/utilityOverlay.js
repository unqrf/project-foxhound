/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Services = object with smart getters for common XPCOM services
var { AppConstants } = ChromeUtils.importESModule(
  "resource://gre/modules/AppConstants.sys.mjs"
);
var { XPCOMUtils } = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
);

ChromeUtils.defineESModuleGetters(this, {
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
  ContextualIdentityService:
    "resource://gre/modules/ContextualIdentityService.sys.mjs",
  PrivateBrowsingUtils: "resource://gre/modules/PrivateBrowsingUtils.sys.mjs",
  URILoadingHelper: "resource:///modules/URILoadingHelper.sys.mjs",
});

XPCOMUtils.defineLazyModuleGetters(this, {
  AboutNewTab: "resource:///modules/AboutNewTab.jsm",
  BrowserWindowTracker: "resource:///modules/BrowserWindowTracker.jsm",
  ExtensionSettingsStore: "resource://gre/modules/ExtensionSettingsStore.jsm",
  ShellService: "resource:///modules/ShellService.jsm",
});

XPCOMUtils.defineLazyGetter(this, "ReferrerInfo", () =>
  Components.Constructor(
    "@mozilla.org/referrer-info;1",
    "nsIReferrerInfo",
    "init"
  )
);

Object.defineProperty(this, "BROWSER_NEW_TAB_URL", {
  enumerable: true,
  get() {
    if (PrivateBrowsingUtils.isWindowPrivate(window)) {
      if (
        !PrivateBrowsingUtils.permanentPrivateBrowsing &&
        !AboutNewTab.newTabURLOverridden
      ) {
        return "about:privatebrowsing";
      }
      // If an extension controls the setting and does not have private
      // browsing permission, use the default setting.
      let extensionControlled = Services.prefs.getBoolPref(
        "browser.newtab.extensionControlled",
        false
      );
      let privateAllowed = Services.prefs.getBoolPref(
        "browser.newtab.privateAllowed",
        false
      );
      // There is a potential on upgrade that the prefs are not set yet, so we double check
      // for moz-extension.
      if (
        !privateAllowed &&
        (extensionControlled ||
          AboutNewTab.newTabURL.startsWith("moz-extension://"))
      ) {
        return "about:privatebrowsing";
      }
    }
    return AboutNewTab.newTabURL;
  },
});

var TAB_DROP_TYPE = "application/x-moz-tabbrowser-tab";

var gBidiUI = false;

/**
 * Determines whether the given url is considered a special URL for new tabs.
 */
function isBlankPageURL(aURL) {
  return (
    aURL == "about:blank" ||
    aURL == "about:home" ||
    aURL == BROWSER_NEW_TAB_URL ||
    aURL == "chrome://browser/content/blanktab.html"
  );
}

function doGetProtocolFlags(aURI) {
  return Services.io.getDynamicProtocolFlags(aURI);
}

function openUILink(
  url,
  event,
  aIgnoreButton,
  aIgnoreAlt,
  aAllowThirdPartyFixup,
  aPostData,
  aReferrerInfo
) {
  return URILoadingHelper.openUILink(
    window,
    url,
    event,
    aIgnoreButton,
    aIgnoreAlt,
    aAllowThirdPartyFixup,
    aPostData,
    aReferrerInfo
  );
}

// This is here for historical reasons. bug 1742889 covers cleaning this up.
function getRootEvent(aEvent) {
  return BrowserUtils.getRootEvent(aEvent);
}

// This is here for historical reasons. bug 1742889 covers cleaning this up.
function whereToOpenLink(e, ignoreButton, ignoreAlt) {
  return BrowserUtils.whereToOpenLink(e, ignoreButton, ignoreAlt);
}

function openTrustedLinkIn(url, where, params) {
  URILoadingHelper.openTrustedLinkIn(window, url, where, params);
}

function openWebLinkIn(url, where, params) {
  URILoadingHelper.openWebLinkIn(window, url, where, params);
}

function openLinkIn(url, where, params) {
  return URILoadingHelper.openLinkIn(window, url, where, params);
}

// Used as an onclick handler for UI elements with link-like behavior.
// e.g. onclick="checkForMiddleClick(this, event);"
// Not needed for menuitems because those fire command events even on middle clicks.
function checkForMiddleClick(node, event) {
  // We should be using the disabled property here instead of the attribute,
  // but some elements that this function is used with don't support it (e.g.
  // menuitem).
  if (node.getAttribute("disabled") == "true") {
    return;
  } // Do nothing

  if (event.target.tagName == "menuitem") {
    // Menu items fire command on middle-click by themselves.
    return;
  }

  if (event.button == 1) {
    /* Execute the node's oncommand or command.
     */

    let cmdEvent = document.createEvent("xulcommandevent");
    cmdEvent.initCommandEvent(
      "command",
      true,
      true,
      window,
      0,
      event.ctrlKey,
      event.altKey,
      event.shiftKey,
      event.metaKey,
      0,
      event,
      event.mozInputSource
    );
    node.dispatchEvent(cmdEvent);

    // Stop the propagation of the click event, to prevent the event from being
    // handled more than once.
    // E.g. see https://bugzilla.mozilla.org/show_bug.cgi?id=1657992#c4
    event.stopPropagation();
    event.preventDefault();

    // If the middle-click was on part of a menu, close the menu.
    // (Menus close automatically with left-click but not with middle-click.)
    closeMenus(event.target);
  }
}

// Populate a menu with user-context menu items. This method should be called
// by onpopupshowing passing the event as first argument.
function createUserContextMenu(
  event,
  {
    isContextMenu = false,
    excludeUserContextId = 0,
    showDefaultTab = false,
    useAccessKeys = true,
  } = {}
) {
  while (event.target.hasChildNodes()) {
    event.target.firstChild.remove();
  }

  let bundle = Services.strings.createBundle(
    "chrome://browser/locale/browser.properties"
  );
  let docfrag = document.createDocumentFragment();

  // If we are excluding a userContextId, we want to add a 'no-container' item.
  if (excludeUserContextId || showDefaultTab) {
    let menuitem = document.createXULElement("menuitem");
    menuitem.setAttribute("data-usercontextid", "0");
    menuitem.setAttribute(
      "label",
      bundle.GetStringFromName("userContextNone.label")
    );
    menuitem.setAttribute(
      "accesskey",
      bundle.GetStringFromName("userContextNone.accesskey")
    );

    if (!isContextMenu) {
      menuitem.setAttribute("command", "Browser:NewUserContextTab");
    }

    docfrag.appendChild(menuitem);

    let menuseparator = document.createXULElement("menuseparator");
    docfrag.appendChild(menuseparator);
  }

  ContextualIdentityService.getPublicIdentities().forEach(identity => {
    if (identity.userContextId == excludeUserContextId) {
      return;
    }

    let menuitem = document.createXULElement("menuitem");
    menuitem.setAttribute("data-usercontextid", identity.userContextId);
    menuitem.setAttribute(
      "label",
      ContextualIdentityService.getUserContextLabel(identity.userContextId)
    );

    if (identity.accessKey && useAccessKeys) {
      menuitem.setAttribute(
        "accesskey",
        bundle.GetStringFromName(identity.accessKey)
      );
    }

    menuitem.classList.add("menuitem-iconic");
    menuitem.classList.add("identity-color-" + identity.color);

    if (!isContextMenu) {
      menuitem.setAttribute("command", "Browser:NewUserContextTab");
    }

    menuitem.classList.add("identity-icon-" + identity.icon);

    docfrag.appendChild(menuitem);
  });

  if (!isContextMenu) {
    docfrag.appendChild(document.createXULElement("menuseparator"));

    let menuitem = document.createXULElement("menuitem");
    menuitem.setAttribute(
      "label",
      bundle.GetStringFromName("userContext.aboutPage.label")
    );
    if (useAccessKeys) {
      menuitem.setAttribute(
        "accesskey",
        bundle.GetStringFromName("userContext.aboutPage.accesskey")
      );
    }
    menuitem.setAttribute("command", "Browser:OpenAboutContainers");
    docfrag.appendChild(menuitem);
  }

  event.target.appendChild(docfrag);
  return true;
}

// Closes all popups that are ancestors of the node.
function closeMenus(node) {
  if ("tagName" in node) {
    if (
      node.namespaceURI ==
        "http://www.mozilla.org/keymaster/gatekeeper/there.is.only.xul" &&
      (node.tagName == "menupopup" || node.tagName == "popup")
    ) {
      node.hidePopup();
    }

    closeMenus(node.parentNode);
  }
}

/** This function takes in a key element and compares it to the keys pressed during an event.
 *
 * @param aEvent
 *        The KeyboardEvent event you want to compare against your key.
 *
 * @param aKey
 *        The <key> element checked to see if it was called in aEvent.
 *        For example, aKey can be a variable set to document.getElementById("key_close")
 *        to check if the close command key was pressed in aEvent.
 */
function eventMatchesKey(aEvent, aKey) {
  let keyPressed = aKey.getAttribute("key").toLowerCase();
  let keyModifiers = aKey.getAttribute("modifiers");
  let modifiers = ["Alt", "Control", "Meta", "Shift"];

  if (aEvent.key != keyPressed) {
    return false;
  }
  let eventModifiers = modifiers.filter(modifier =>
    aEvent.getModifierState(modifier)
  );
  // Check if aEvent has a modifier and aKey doesn't
  if (eventModifiers.length && !keyModifiers.length) {
    return false;
  }
  // Check whether aKey's modifiers match aEvent's modifiers
  if (keyModifiers) {
    keyModifiers = keyModifiers.split(/[\s,]+/);
    // Capitalize first letter of aKey's modifers to compare to aEvent's modifier
    keyModifiers.forEach(function(modifier, index) {
      if (modifier == "accel") {
        keyModifiers[index] =
          AppConstants.platform == "macosx" ? "Meta" : "Control";
      } else {
        keyModifiers[index] = modifier[0].toUpperCase() + modifier.slice(1);
      }
    });
    return modifiers.every(
      modifier =>
        keyModifiers.includes(modifier) == aEvent.getModifierState(modifier)
    );
  }
  return true;
}

// Gather all descendent text under given document node.
function gatherTextUnder(root) {
  var text = "";
  var node = root.firstChild;
  var depth = 1;
  while (node && depth > 0) {
    // See if this node is text.
    if (node.nodeType == Node.TEXT_NODE) {
      // Add this text to our collection.
      text += " " + node.data;
    } else if (HTMLImageElement.isInstance(node)) {
      // If it has an "alt" attribute, add that.
      var altText = node.getAttribute("alt");
      if (altText && altText != "") {
        text += " " + altText;
      }
    }
    // Find next node to test.
    // First, see if this node has children.
    if (node.hasChildNodes()) {
      // Go to first child.
      node = node.firstChild;
      depth++;
    } else {
      // No children, try next sibling (or parent next sibling).
      while (depth > 0 && !node.nextSibling) {
        node = node.parentNode;
        depth--;
      }
      if (node.nextSibling) {
        node = node.nextSibling;
      }
    }
  }
  // Strip leading and tailing whitespace.
  text = text.trim();
  // Compress remaining whitespace.
  text = text.replace(/\s+/g, " ");
  return text;
}

// This function exists for legacy reasons.
function getShellService() {
  return ShellService;
}

function isBidiEnabled() {
  // first check the pref.
  if (Services.prefs.getBoolPref("bidi.browser.ui", false)) {
    return true;
  }

  // now see if the app locale is an RTL one.
  const isRTL = Services.locale.isAppLocaleRTL;

  if (isRTL) {
    Services.prefs.setBoolPref("bidi.browser.ui", true);
  }
  return isRTL;
}

function openAboutDialog() {
  for (let win of Services.wm.getEnumerator("Browser:About")) {
    // Only open one about window (Bug 599573)
    if (win.closed) {
      continue;
    }
    win.focus();
    return;
  }

  var features = "chrome,";
  if (AppConstants.platform == "win") {
    features += "centerscreen,dependent";
  } else if (AppConstants.platform == "macosx") {
    features += "centerscreen,resizable=no,minimizable=no";
  } else {
    features += "centerscreen,dependent,dialog=no";
  }

  window.openDialog("chrome://browser/content/aboutDialog.xhtml", "", features);
}

async function openPreferences(paneID, extraArgs) {
  // This function is duplicated from preferences.js.
  function internalPrefCategoryNameToFriendlyName(aName) {
    return (aName || "").replace(/^pane./, function(toReplace) {
      return toReplace[4].toLowerCase();
    });
  }

  let win = Services.wm.getMostRecentWindow("navigator:browser");
  let friendlyCategoryName = internalPrefCategoryNameToFriendlyName(paneID);
  let params;
  if (extraArgs && extraArgs.urlParams) {
    params = new URLSearchParams();
    let urlParams = extraArgs.urlParams;
    for (let name in urlParams) {
      if (urlParams[name] !== undefined) {
        params.set(name, urlParams[name]);
      }
    }
  }
  let preferencesURL =
    "about:preferences" +
    (params ? "?" + params : "") +
    (friendlyCategoryName ? "#" + friendlyCategoryName : "");
  let newLoad = true;
  let browser = null;
  if (!win) {
    let windowArguments = Cc["@mozilla.org/array;1"].createInstance(
      Ci.nsIMutableArray
    );
    let supportsStringPrefURL = Cc[
      "@mozilla.org/supports-string;1"
    ].createInstance(Ci.nsISupportsString);
    supportsStringPrefURL.data = preferencesURL;
    windowArguments.appendElement(supportsStringPrefURL);

    win = Services.ww.openWindow(
      null,
      AppConstants.BROWSER_CHROME_URL,
      "_blank",
      "chrome,dialog=no,all",
      windowArguments
    );
  } else {
    let shouldReplaceFragment = friendlyCategoryName
      ? "whenComparingAndReplace"
      : "whenComparing";
    newLoad = !win.switchToTabHavingURI(preferencesURL, true, {
      ignoreFragment: shouldReplaceFragment,
      replaceQueryString: true,
      triggeringPrincipal: Services.scriptSecurityManager.getSystemPrincipal(),
    });
    browser = win.gBrowser.selectedBrowser;
  }

  if (!newLoad && paneID) {
    if (browser.contentDocument?.readyState != "complete") {
      await new Promise(resolve => {
        browser.addEventListener("load", resolve, {
          capture: true,
          once: true,
        });
      });
    }
    browser.contentWindow.gotoPref(paneID);
  }
}

/**
 * Opens the troubleshooting information (about:support) page for this version
 * of the application.
 */
function openTroubleshootingPage() {
  openTrustedLinkIn("about:support", "tab");
}

/**
 * Opens the feedback page for this version of the application.
 */
function openFeedbackPage() {
  var url = Services.urlFormatter.formatURLPref("app.feedback.baseURL");
  openTrustedLinkIn(url, "tab");
}

function buildHelpMenu() {
  document.getElementById(
    "feedbackPage"
  ).disabled = !Services.policies.isAllowed("feedbackCommands");

  document.getElementById(
    "helpSafeMode"
  ).disabled = !Services.policies.isAllowed("safeMode");

  document.getElementById(
    "troubleShooting"
  ).disabled = !Services.policies.isAllowed("aboutSupport");

  let supportMenu = Services.policies.getSupportMenu();
  if (supportMenu) {
    let menuitem = document.getElementById("helpPolicySupport");
    menuitem.hidden = false;
    menuitem.setAttribute("label", supportMenu.Title);
    if ("AccessKey" in supportMenu) {
      menuitem.setAttribute("accesskey", supportMenu.AccessKey);
    }
    document.getElementById("helpPolicySeparator").hidden = false;
  }

  // Enable/disable the "Report Web Forgery" menu item.
  if (typeof gSafeBrowsing != "undefined") {
    gSafeBrowsing.setReportPhishingMenu();
  }

  // We're testing to see if the WebCompat team's "Report Site Issue"
  // access point makes sense in the Help menu. Normally checking this
  // pref wouldn't be enough, since there's also the case that the
  // add-on has somehow been disabled by the user or third-party software
  // without flipping the pref. Since this add-on is only used on pre-release
  // channels, and since the jury is still out on whether or not the Help menu
  // is the right place for this item, we're going to do a least-effort
  // approach here and assume that the pref is enough to determine whether the
  // menuitem should appear.
  //
  // See bug 1690573 for further details.
  let reportSiteIssueEnabled = Services.prefs.getBoolPref(
    "extensions.webcompat-reporter.enabled",
    false
  );
  let reportSiteIssue = document.getElementById("help_reportSiteIssue");
  reportSiteIssue.hidden = !reportSiteIssueEnabled;
  if (reportSiteIssueEnabled) {
    let uri = gBrowser.currentURI;
    let isReportablePage =
      uri && (uri.schemeIs("http") || uri.schemeIs("https"));
    reportSiteIssue.disabled = !isReportablePage;
  }
}

function isElementVisible(aElement) {
  if (!aElement) {
    return false;
  }

  // If aElement or a direct or indirect parent is hidden or collapsed,
  // height, width or both will be 0.
  var rect = aElement.getBoundingClientRect();
  return rect.height > 0 && rect.width > 0;
}

function makeURLAbsolute(aBase, aUrl) {
  // Note:  makeURI() will throw if aUri is not a valid URI
  return makeURI(aUrl, null, makeURI(aBase)).spec;
}

function getHelpLinkURL(aHelpTopic) {
  var url = Services.urlFormatter.formatURLPref("app.support.baseURL");
  return url + aHelpTopic;
}

// aCalledFromModal is optional
function openHelpLink(aHelpTopic, aCalledFromModal, aWhere) {
  var url = getHelpLinkURL(aHelpTopic);
  var where = aWhere;
  if (!aWhere) {
    where = aCalledFromModal ? "window" : "tab";
  }

  openTrustedLinkIn(url, where);
}

function openPrefsHelp(aEvent) {
  let helpTopic = aEvent.target.getAttribute("helpTopic");
  openHelpLink(helpTopic);
}
