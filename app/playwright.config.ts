import { defineConfig } from "@playwright/test";

// Integration tests against PX4 SITL. Each run starts fresh SITL vehicles
// (tests/global-setup.ts), a fresh backend (through the dev server), and
// records video and screenshots into .falconer/test-results.
export default defineConfig({
  testDir: "tests",
  timeout: 300_000,
  expect: { timeout: 30_000 },
  workers: 1,
  retries: 0,
  reporter: [["list"], ["html", { open: "never", outputFolder: "../.falconer/test-report" }]],
  outputDir: "../.falconer/test-results",
  globalSetup: "./tests/global-setup.ts",
  globalTeardown: "./tests/global-teardown.ts",
  use: {
    baseURL: "http://127.0.0.1:5173",
    channel: "chrome",
    viewport: { width: 1400, height: 900 },
    video: "on",
    screenshot: "on",
    trace: "retain-on-failure",
  },
  webServer: {
    command: "npm run dev",
    url: "http://127.0.0.1:5173",
    reuseExistingServer: false,
    timeout: 60_000,
  },
});
