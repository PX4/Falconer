import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { expect, test, type Page } from "@playwright/test";
import { repoRoot } from "./sitl";

// Flies the example mission on one SITL vehicle entirely through the web UI
// and checks that the map follows the vehicle along the planned route.

const ID = "v1";
const PLAN = resolve(repoRoot, "missions/px4-multicopter-example.plan");

interface LatLon {
  lat: number;
  lon: number;
}

// Local flat-earth approximation, fine over a few kilometres.
function toMeters(origin: LatLon, p: LatLon): [number, number] {
  const k = 111_320;
  return [(p.lon - origin.lon) * k * Math.cos((origin.lat * Math.PI) / 180), (p.lat - origin.lat) * k];
}

function distance(a: LatLon, b: LatLon): number {
  const [x, y] = toMeters(a, b);
  return Math.hypot(x, y);
}

function distanceToRoute(p: LatLon, route: LatLon[]): number {
  let best = Infinity;
  for (let i = 0; i + 1 < route.length; i++) {
    const [ax, ay] = toMeters(p, route[i]);
    const [bx, by] = toMeters(p, route[i + 1]);
    const dx = bx - ax;
    const dy = by - ay;
    const t = Math.max(0, Math.min(1, -(ax * dx + ay * dy) / (dx * dx + dy * dy || 1)));
    best = Math.min(best, Math.hypot(ax + t * dx, ay + t * dy));
  }
  return best;
}

function plannedRoute(): LatLon[] {
  const plan = JSON.parse(readFileSync(PLAN, "utf8"));
  const home = plan.mission.plannedHomePosition;
  const points: LatLon[] = [{ lat: home[0], lon: home[1] }];
  for (const item of plan.mission.items) {
    const p = item.params ?? [];
    if (p[4] || p[5]) points.push({ lat: p[4], lon: p[5] });
  }
  return points;
}

async function markerPosition(page: Page): Promise<LatLon> {
  const marker = page.getByTestId(`marker-${ID}`);
  return {
    lat: Number(await marker.getAttribute("data-lat")),
    lon: Number(await marker.getAttribute("data-lon")),
  };
}

async function panelPosition(page: Page): Promise<LatLon> {
  return {
    lat: Number(await page.getByTestId(`${ID}-lat`).textContent()),
    lon: Number(await page.getByTestId(`${ID}-lon`).textContent()),
  };
}

async function missionItem(page: Page): Promise<number> {
  const text = (await page.getByTestId(`${ID}-mission`).textContent()) ?? "";
  const match = /^\s*(\d+)\s*\//.exec(text);
  return match ? Number(match[1]) : -1;
}

test("example mission flies and the map follows the vehicle", async ({ page }) => {
  const route = plannedRoute();

  await page.goto("/");
  await expect(page.getByTestId("connection")).toHaveText("connected");

  await test.step("vehicle appears and becomes armable", async () => {
    await expect(page.getByTestId(`vehicle-${ID}`)).toBeVisible({ timeout: 60_000 });
    await expect(page.getByTestId(`marker-${ID}`)).toBeAttached();
    await expect(page.getByTestId(`${ID}-armable`)).toHaveText("yes", { timeout: 90_000 });
  });

  await test.step("upload, arm, start", async () => {
    await page.getByTestId("load-mission").click();
    // The map engine loaded the route: one line plus a point per waypoint
    // (features crossing tile edges can be counted more than once).
    const waypoints = plannedRoute().length - 1; // minus home
    await expect
      .poll(async () => Number(await page.getByTestId("map").getAttribute("data-route-features")))
      .toBeGreaterThanOrEqual(waypoints + 1);
    await page.getByTestId("upload-mission").click();
    await expect(page.getByTestId("last-result")).toHaveText(`upload: ${ID} ok`);
    await page.getByTestId("arm").click();
    await expect(page.getByTestId("last-result")).toHaveText(`arm: ${ID} ok`);
    await page.getByTestId("start-mission").click();
    await expect(page.getByTestId("last-result")).toHaveText(`start: ${ID} ok`);
    await expect(page.getByTestId(`${ID}-mode`)).toHaveText("Mission");
  });

  await test.step("climbs to mission altitude", async () => {
    await expect
      .poll(async () => Number(await page.getByTestId(`${ID}-alt`).textContent()), {
        timeout: 60_000,
      })
      .toBeGreaterThan(15);
  });

  const start = await markerPosition(page);

  await test.step("marker moves at least 150 m along the route", async () => {
    await expect
      .poll(async () => distance(start, await markerPosition(page)), {
        timeout: 120_000,
        intervals: [1000],
      })
      .toBeGreaterThan(150);
  });

  await test.step("mission progresses past the first waypoint", async () => {
    await expect.poll(() => missionItem(page), { timeout: 120_000 }).toBeGreaterThanOrEqual(2);
  });

  await test.step("marker stays on the planned route and matches telemetry", async () => {
    const marker = await markerPosition(page);
    expect(distanceToRoute(marker, route)).toBeLessThan(50);
    expect(distance(marker, await panelPosition(page))).toBeLessThan(20);
  });

  await page.getByTestId("rtl").click();
  await expect(page.getByTestId("last-result")).toHaveText(`rtl: ${ID} ok`);
});
