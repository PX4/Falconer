// Minimal reader for QGroundControl .plan files: enough to draw the route.

export interface Waypoint {
  seq: number;
  command: number;
  lat: number;
  lon: number;
  alt: number;
}

export interface Plan {
  text: string;
  home?: { lat: number; lon: number };
  waypoints: Waypoint[];
}

// MAV_CMD values that carry a position in params 5-7.
const NAV_COMMANDS = new Set([16, 17, 18, 19, 21, 22, 82, 84, 85]);

export function parsePlan(text: string): Plan {
  const json = JSON.parse(text);
  const mission = json.mission ?? {};
  const waypoints: Waypoint[] = [];
  let seq = 0;
  for (const item of mission.items ?? []) {
    if (item.type !== "SimpleItem") continue;
    const p = item.params ?? [];
    if (NAV_COMMANDS.has(item.command) && (p[4] !== 0 || p[5] !== 0)) {
      waypoints.push({ seq, command: item.command, lat: p[4], lon: p[5], alt: p[6] });
    }
    seq++;
  }
  const home = mission.plannedHomePosition;
  return {
    text,
    home: Array.isArray(home) ? { lat: home[0], lon: home[1] } : undefined,
    waypoints,
  };
}
