/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

/**
 * Tests that the progress strings that the Migration Wizard shows
 * during migrations for IE and Edge uses the term "Favorites" rather
 * then "Bookmarks".
 */
add_task(async function test_ie_edge_bookmarks_success_strings() {
  for (let key of ["ie", "edge", "internal-testing"]) {
    let sandbox = sinon.createSandbox();

    sandbox.stub(InternalTestingProfileMigrator, "key").get(() => {
      return key;
    });

    sandbox.stub(MigrationUtils, "availableMigratorKeys").get(() => {
      return key;
    });

    let testingMigrator = new InternalTestingProfileMigrator();
    sandbox.stub(MigrationUtils, "getMigrator").callsFake(() => {
      return Promise.resolve(testingMigrator);
    });

    let migration = waitForTestMigration(
      [MigrationUtils.resourceTypes.BOOKMARKS],
      [MigrationUtils.resourceTypes.BOOKMARKS],
      InternalTestingProfileMigrator.testProfile
    );

    await withMigrationWizardDialog(async prefsWin => {
      let dialogBody = prefsWin.document.body;
      let wizard = dialogBody.querySelector("migration-wizard");
      let wizardDone = BrowserTestUtils.waitForEvent(
        wizard,
        "MigrationWizard:DoneMigration"
      );
      selectResourceTypesAndStartMigration(
        wizard,
        [MigrationWizardConstants.DISPLAYED_RESOURCE_TYPES.BOOKMARKS],
        key
      );
      await migration;

      let dialog = prefsWin.document.querySelector("#migrationWizardDialog");
      let shadow = wizard.openOrClosedShadowRoot;

      // If we were using IE or Edge (EdgeHTLM), then the success message should
      // include the word "favorites". Otherwise, we expect it to include
      // the word "bookmarks".
      let bookmarksProgressGroup = shadow.querySelector(
        `.resource-progress-group[data-resource-type="${MigrationWizardConstants.DISPLAYED_RESOURCE_TYPES.BOOKMARKS}"`
      );
      let successTextElement = bookmarksProgressGroup.querySelector(
        ".success-text"
      );

      await BrowserTestUtils.waitForCondition(() => {
        return successTextElement.textContent.trim();
      });

      let successText = successTextElement.textContent.toLowerCase();

      if (key == "internal-testing") {
        Assert.ok(
          successText.includes("bookmarks"),
          `Success test should refer to bookmarks: ${successText}.`
        );
      } else {
        Assert.ok(
          successText.includes("favorites"),
          `Success test should refer to favorites: ${successText}`
        );
      }

      let doneButton = shadow.querySelector("#done-button");
      let dialogClosed = BrowserTestUtils.waitForEvent(dialog, "close");
      doneButton.click();
      await dialogClosed;
      await wizardDone;
    });

    sandbox.restore();
  }
});
