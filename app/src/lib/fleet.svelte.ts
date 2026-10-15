// Reactive fleet state for the panels. Map markers do not read this: they
// are fed straight from the stream (see MapView) to keep telemetry off the
// render path.

import type { VehicleInfo, VehicleState } from "../gen/falconer/v1/contract_pb";

function idLess(a: string, b: string): number {
  return a.length - b.length || a.localeCompare(b);
}

class Fleet {
  status = $state<"connecting" | "connected" | "error">("connecting");
  error = $state("");
  vehicles = $state.raw<VehicleInfo[]>([]);
  states = $state.raw<Record<string, VehicleState>>({});
  lastResult = $state("");

  setVehicle(info: VehicleInfo) {
    const others = this.vehicles.filter((v) => v.id !== info.id);
    this.vehicles = [...others, info].sort((a, b) => idLess(a.id, b.id));
  }

  setStates(states: VehicleState[]) {
    if (states.length === 0) return;
    const next = { ...this.states };
    for (const s of states) next[s.id] = s;
    this.states = next;
  }

  get ids(): string[] {
    return this.vehicles.map((v) => v.id);
  }
}

export const fleet = new Fleet();
