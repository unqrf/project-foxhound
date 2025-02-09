"use strict";

const { AboutWelcomeParent } = ChromeUtils.import(
  "resource:///actors/AboutWelcomeParent.jsm"
);

const { AboutWelcomeTelemetry } = ChromeUtils.import(
  "resource://activity-stream/aboutwelcome/lib/AboutWelcomeTelemetry.jsm"
);
const { AWScreenUtils } = ChromeUtils.import(
  "resource://activity-stream/lib/AWScreenUtils.jsm"
);

async function clickVisibleButton(browser, selector) {
  // eslint-disable-next-line no-shadow
  await ContentTask.spawn(browser, { selector }, async ({ selector }) => {
    function getVisibleElement() {
      for (const el of content.document.querySelectorAll(selector)) {
        if (el.offsetParent !== null) {
          return el;
        }
      }
      return null;
    }
    await ContentTaskUtils.waitForCondition(
      getVisibleElement,
      selector,
      200, // interval
      100 // maxTries
    );
    getVisibleElement().click();
  });
}

function initSandbox({ pin = true, isDefault = false } = {}) {
  const sandbox = sinon.createSandbox();
  sandbox.stub(AboutWelcomeParent, "doesAppNeedPin").returns(pin);
  sandbox.stub(AboutWelcomeParent, "isDefaultBrowser").returns(isDefault);

  return sandbox;
}

/**
 * Test MR message telemetry
 */
add_task(async function test_aboutwelcome_mr_template_telemetry() {
  const sandbox = initSandbox();

  let { browser, cleanup } = await openMRAboutWelcome();
  let aboutWelcomeActor = await getAboutWelcomeParent(browser);
  // Stub AboutWelcomeParent's Content Message Handler
  const messageStub = sandbox.spy(aboutWelcomeActor, "onContentMessage");
  await clickVisibleButton(browser, ".action-buttons button.secondary");

  const { callCount } = messageStub;
  ok(callCount >= 1, `${callCount} Stub was called`);
  let clickCall;
  for (let i = 0; i < callCount; i++) {
    const call = messageStub.getCall(i);
    info(`Call #${i}: ${call.args[0]} ${JSON.stringify(call.args[1])}`);
    if (call.calledWithMatch("", { event: "CLICK_BUTTON" })) {
      clickCall = call;
    }
  }

  Assert.ok(
    clickCall.args[1].message_id.startsWith("MR_WELCOME_DEFAULT"),
    "Telemetry includes MR message id"
  );

  await cleanup();
  sandbox.restore();
});

/**
 * Telemetry Impression with Pin as First Screen
 */
add_task(async function test_aboutwelcome_pin_screen_impression() {
  await pushPrefs(["browser.shell.checkDefaultBrowser", true]);

  const sandbox = initSandbox();
  sandbox
    .stub(AWScreenUtils, "evaluateScreenTargeting")
    .resolves(true)
    .withArgs(
      "os.windowsBuildNumber >= 15063 && !isDefaultBrowser && !doesAppNeedPin"
    )
    .resolves(false);

  let impressionSpy = sandbox.spy(
    AboutWelcomeTelemetry.prototype,
    "sendTelemetry"
  );

  let { browser, cleanup } = await openMRAboutWelcome();
  // Wait for screen elements to render before checking impression pings
  await test_screen_content(
    browser,
    "Onboarding screen elements rendered",
    // Expected selectors:
    [
      `main.screen[pos="split"]`,
      "div.secondary-cta.top",
      "button[value='secondary_button_top']",
    ]
  );

  const { callCount } = impressionSpy;
  ok(callCount >= 1, `${callCount} impressionSpy was called`);
  let impressionCall;
  for (let i = 0; i < callCount; i++) {
    const call = impressionSpy.getCall(i);
    info(`Call #${i}:  ${JSON.stringify(call.args[0])}`);
    if (
      call.calledWithMatch({ event: "IMPRESSION" }) &&
      !call.calledWithMatch({ message_id: "MR_WELCOME_DEFAULT" })
    ) {
      info(`Screen Impression Call #${i}:  ${JSON.stringify(call.args[0])}`);
      impressionCall = call;
    }
  }

  Assert.ok(
    impressionCall.args[0].message_id.startsWith(
      "MR_WELCOME_DEFAULT_0_AW_PIN_FIREFOX_P"
    ),
    "Impression telemetry includes correct message id"
  );
  await cleanup();
  sandbox.restore();
  await popPrefs();
});

/**
 * Test MR template content - Browser is not Pinned and not set as default
 */
add_task(async function test_aboutwelcome_mr_template_content() {
  await pushPrefs(["browser.shell.checkDefaultBrowser", true]);

  const sandbox = initSandbox();

  sandbox
    .stub(AWScreenUtils, "evaluateScreenTargeting")
    .resolves(true)
    .withArgs(
      "os.windowsBuildNumber >= 15063 && !isDefaultBrowser && !doesAppNeedPin"
    )
    .resolves(false);

  let { cleanup, browser } = await openMRAboutWelcome();

  await test_screen_content(
    browser,
    "MR template includes screens with split position and a sign in link on the first screen",
    // Expected selectors:
    [
      `main.screen[pos="split"]`,
      "div.secondary-cta.top",
      "button[value='secondary_button_top']",
    ]
  );

  await test_screen_content(
    browser,
    "renders pin screen",
    //Expected selectors:
    ["main.AW_PIN_FIREFOX"],
    //Unexpected selectors:
    ["main.AW_GRATITUDE"]
  );

  await clickVisibleButton(browser, ".action-buttons button.secondary");

  //should render set default
  await test_screen_content(
    browser,
    "renders set default screen",
    //Expected selectors:
    ["main.AW_SET_DEFAULT"],
    //Unexpected selectors:
    ["main.AW_CHOOSE_THEME"]
  );

  await cleanup();
  sandbox.restore();
  await popPrefs();
});

/**
 * Test MR template content - Browser has been set as Default, not pinned
 */
add_task(async function test_aboutwelcome_mr_template_content_pin() {
  await pushPrefs(["browser.shell.checkDefaultBrowser", true]);

  const sandbox = initSandbox({ isDefault: true });

  sandbox
    .stub(AWScreenUtils, "evaluateScreenTargeting")
    .resolves(true)
    .withArgs(
      "os.windowsBuildNumber >= 15063 && !isDefaultBrowser && !doesAppNeedPin"
    )
    .resolves(false);

  let { browser, cleanup } = await openMRAboutWelcome();

  await test_screen_content(
    browser,
    "renders pin screen",
    //Expected selectors:
    ["main.AW_PIN_FIREFOX"],
    //Unexpected selectors:
    ["main.AW_SET_DEFAULT"]
  );

  await clickVisibleButton(browser, ".action-buttons button.secondary");

  await test_screen_content(
    browser,
    "renders next screen",
    //Expected selectors:
    ["main"],
    //Unexpected selectors:
    ["main.AW_SET_DEFAULT"]
  );

  await cleanup();
  sandbox.restore();
  await popPrefs();
});

/**
 * Test MR template content - Browser is Pinned, not default
 */
add_task(async function test_aboutwelcome_mr_template_only_default() {
  await pushPrefs(["browser.shell.checkDefaultBrowser", true]);

  const sandbox = initSandbox({ pin: false });
  sandbox
    .stub(AWScreenUtils, "evaluateScreenTargeting")
    .resolves(true)
    .withArgs(
      "os.windowsBuildNumber >= 15063 && !isDefaultBrowser && !doesAppNeedPin"
    )
    .resolves(false);

  let { browser, cleanup } = await openMRAboutWelcome();
  //should render set default
  await test_screen_content(
    browser,
    "renders set default screen",
    //Expected selectors:
    ["main.AW_ONLY_DEFAULT"],
    //Unexpected selectors:
    ["main.AW_PIN_FIREFOX"]
  );

  await cleanup();
  sandbox.restore();
  await popPrefs();
});
/**
 * Test MR template content - Browser is Pinned and set as default
 */
add_task(async function test_aboutwelcome_mr_template_get_started() {
  await pushPrefs(["browser.shell.checkDefaultBrowser", true]);

  const sandbox = initSandbox({ pin: false, isDefault: true });

  sandbox
    .stub(AWScreenUtils, "evaluateScreenTargeting")
    .resolves(true)
    .withArgs(
      "os.windowsBuildNumber >= 15063 && !isDefaultBrowser && !doesAppNeedPin"
    )
    .resolves(false);

  let { browser, cleanup } = await openMRAboutWelcome();

  //should render set default
  await test_screen_content(
    browser,
    "doesn't render pin and set default screens",
    //Expected selectors:
    ["main.AW_GET_STARTED"],
    //Unexpected selectors:
    ["main.AW_PIN_FIREFOX", "main.AW_ONLY_DEFAULT"]
  );

  await cleanup();
  sandbox.restore();
  await popPrefs();
});

add_task(async function test_aboutwelcome_gratitude() {
  const TEST_CONTENT = [
    {
      id: "AW_GRATITUDE",
      content: {
        position: "split",
        split_narrow_bkg_position: "-228px",
        background:
          "url('chrome://activity-stream/content/data/content/assets/mr-gratitude.svg') var(--mr-secondary-position) no-repeat, var(--mr-screen-background-color)",
        progress_bar: true,
        logo: {},
        title: {
          string_id: "mr2022-onboarding-gratitude-title",
        },
        subtitle: {
          string_id: "mr2022-onboarding-gratitude-subtitle",
        },
        primary_button: {
          label: {
            string_id: "mr2022-onboarding-gratitude-primary-button-label",
          },
          action: {
            navigate: true,
          },
        },
      },
    },
  ];
  await setAboutWelcomeMultiStage(JSON.stringify(TEST_CONTENT)); // NB: calls SpecialPowers.pushPrefEnv
  let { cleanup, browser } = await openMRAboutWelcome();

  // execution
  await test_screen_content(
    browser,
    "doesn't render secondary button on gratitude screen",
    //Expected selectors
    ["main.AW_GRATITUDE", "button[value='primary_button']"],

    //Unexpected selectors:
    ["button[value='secondary_button']"]
  );
  await clickVisibleButton(browser, ".action-buttons button.primary");

  // make sure the button navigates to newtab
  await test_screen_content(
    browser,
    //Expected selectors
    ["body.activity-stream"],

    //Unexpected selectors:
    ["main.AW_GRATITUDE"]
  );

  // cleanup
  await SpecialPowers.popPrefEnv(); // for setAboutWelcomeMultiStage
  await cleanup();
});
