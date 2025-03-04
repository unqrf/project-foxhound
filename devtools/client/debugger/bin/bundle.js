/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

const path = require("path");
var fs = require("fs");
const rimraf = require("rimraf");
const webpack = require("webpack");

const projectPath = path.resolve(__dirname, "..");
const bundlePath = path.join(projectPath, "./dist");
const clientPath = path.join(projectPath, "../");

process.env.NODE_ENV = "production";

function moveFile(src, dest) {
  if (!fs.existsSync(src)) {
    return;
  }

  fs.copyFileSync(src, dest);
  rimraf.sync(src);
}

/**
 * The `bundle` module will build the following:
 * - vendors.js and vendors.css:
 *     Bundle for all the external packages still used by the Debugger frontend.
 *     Source at devtools/client/debugger/src/vendors.js
 * - parser-worker.js, pretty-print-worker.js, search-worker:
 *     Workers used only by the debugger.
 *     Sources at devtools/client/debugger/src/workers/*
 */
(async function bundle() {
  process.env.TARGET = "firefox-panel";
  process.env.OUTPUT_PATH = bundlePath;

  const webpackConfig = require(path.resolve(projectPath, "webpack.config.js"));
  const webpackCompiler = webpack(webpackConfig);

  const result = await new Promise(resolve => {
    webpackCompiler.run((error, stats) => resolve(stats));
  });

  if (result?.hasErrors()) {
    console.log(
      "[bundle] Something went wrong. The error was written to assets-error.log"
    );

    fs.writeFileSync(
      "assets-error.log",
      JSON.stringify(result.toJson("verbose"), null, 2)
    );
    return;
  }

  console.log(`[bundle] Done bundling.`);
})();
