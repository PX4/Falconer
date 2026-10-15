<script lang="ts">
  import { onMount } from "svelte";
  import examplePlan from "../../missions/px4-multicopter-example.plan?raw";
  import { ContractClient, errorText, type Command } from "./lib/contract";
  import { fleet } from "./lib/fleet.svelte";
  import MapView from "./lib/MapView.svelte";
  import { parsePlan, type Plan } from "./lib/plan";
  import { fetchSession } from "./lib/session";

  const client = new ContractClient();
  let plan = $state<Plan | undefined>();
  let busy = $state(false);

  onMount(() => {
    client.onStream((batch) => {
      for (const event of batch.events) {
        if (event.event.case === "vehicleAdded" || event.event.case === "vehicleChanged") {
          fleet.setVehicle(event.event.value);
        }
      }
      fleet.setStates(batch.vehicles);
    });
    client.onClose = (reason) => {
      fleet.status = "error";
      fleet.error = reason;
    };
    void connect();
  });

  async function connect() {
    try {
      // The backend may still be starting.
      let session;
      for (let attempt = 0; ; attempt++) {
        try {
          session = await fetchSession();
          break;
        } catch (e) {
          if (attempt >= 20) throw e;
          await new Promise((r) => setTimeout(r, 500));
        }
      }
      await client.connect(session);
      const list = await client.request({ case: "listVehicles", value: {} });
      if (list.result.case === "vehicleList") {
        for (const v of list.result.value.vehicles) fleet.setVehicle(v);
      }
      await client.request({ case: "subscribe", value: {} });
      fleet.status = "connected";
    } catch (e) {
      fleet.status = "error";
      fleet.error = (e as Error).message;
    }
  }

  async function run(label: string, cmd: Command) {
    busy = true;
    try {
      const res = await client.request(cmd);
      if (res.result.case === "error") {
        fleet.lastResult = `${label}: ${errorText(res.result.value.code, res.result.value.message)}`;
      } else if (res.result.case === "group") {
        const parts = res.result.value.results.map((r) =>
          r.error ? `${r.vehicleId} ${errorText(r.error.code, r.error.message)}` : `${r.vehicleId} ok`,
        );
        fleet.lastResult = `${label}: ${parts.join(", ")}`;
      } else {
        fleet.lastResult = `${label}: ok`;
      }
    } catch (e) {
      fleet.lastResult = `${label}: ${(e as Error).message}`;
    } finally {
      busy = false;
    }
  }

  const vehicleIds = () => fleet.ids;
  const noVehicles = $derived(fleet.vehicles.length === 0);

  function loadExample() {
    plan = parsePlan(examplePlan);
    fleet.lastResult = `loaded example mission (${plan.waypoints.length} waypoints)`;
  }

  const fmt = (v: number | undefined, digits = 1) => (v ?? 0).toFixed(digits);
</script>

<main>
  <MapView {client} {plan} />

  <aside>
    <header>
      <h1>Falconer</h1>
      <span class="status {fleet.status}" data-testid="connection">
        {fleet.status === "error" ? `error: ${fleet.error}` : fleet.status}
      </span>
    </header>

    <section>
      <h2>Mission</h2>
      <div class="buttons">
        <button onclick={loadExample} data-testid="load-mission">Load example</button>
        <button
          disabled={!plan || noVehicles || busy}
          data-testid="upload-mission"
          onclick={() =>
            run("upload", {
              case: "uploadMission",
              value: { vehicleIds: vehicleIds(), qgcPlan: plan!.text },
            })}>Upload</button>
        <button
          disabled={noVehicles || busy}
          data-testid="arm"
          onclick={() => run("arm", { case: "arm", value: { vehicleIds: vehicleIds() } })}
          >Arm</button>
        <button
          disabled={noVehicles || busy}
          data-testid="start-mission"
          onclick={() =>
            run("start", { case: "startMission", value: { vehicleIds: vehicleIds() } })}
          >Start</button>
        <button
          disabled={noVehicles || busy}
          data-testid="rtl"
          onclick={() =>
            run("rtl", { case: "returnToLaunch", value: { vehicleIds: vehicleIds() } })}
          >RTL</button>
      </div>
      <p class="result" data-testid="last-result">{fleet.lastResult}</p>
    </section>

    <section>
      <h2>Vehicles</h2>
      {#if noVehicles}
        <p class="dim">Waiting for vehicles on UDP 14550</p>
      {/if}
      {#each fleet.vehicles as info (info.id)}
        {@const s = fleet.states[info.id]}
        <div class="vehicle" data-testid="vehicle-{info.id}">
          <div class="title">
            <strong>{info.id}</strong>
            <span class="dim">system {info.displaySystemId}</span>
            <span class="pill {info.connected ? 'ok' : 'bad'}">{info.connected ? "link" : "LOST"}</span>
          </div>
          {#if s}
            <dl>
              <dt>Mode</dt><dd data-testid="{info.id}-mode">{s.flightMode}</dd>
              <dt>Armed</dt><dd data-testid="{info.id}-armed">{s.armed ? "yes" : "no"}</dd>
              <dt>Armable</dt><dd data-testid="{info.id}-armable">{s.armable ? "yes" : "no"}</dd>
              <dt>Altitude</dt><dd><span data-testid="{info.id}-alt">{fmt(s.position?.relativeAltitudeM)}</span> m</dd>
              <dt>Speed</dt><dd>{fmt(Math.hypot(s.velocity?.northMS ?? 0, s.velocity?.eastMS ?? 0))} m/s</dd>
              <dt>Battery</dt><dd>{fmt(s.battery?.remainingPercent, 0)}%</dd>
              <dt>Position</dt>
              <dd>
                <span data-testid="{info.id}-lat">{fmt(s.position?.latitudeDeg, 7)}</span>,
                <span data-testid="{info.id}-lon">{fmt(s.position?.longitudeDeg, 7)}</span>
              </dd>
              <dt>Mission</dt>
              <dd data-testid="{info.id}-mission">
                {s.mission && s.mission.total > 0 ? `${s.mission.current} / ${s.mission.total}` : "none"}
              </dd>
            </dl>
            {#if s.armingProblems.length > 0}
              <ul class="problems">
                {#each s.armingProblems as p, i (i)}
                  <li>{p.severity}: {p.message}</li>
                {/each}
              </ul>
            {/if}
          {/if}
        </div>
      {/each}
    </section>
  </aside>
</main>

<style>
  :global(html, body) {
    margin: 0;
    height: 100%;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Helvetica, Arial, sans-serif;
    color: #1f2328;
  }
  main {
    position: fixed;
    inset: 0;
  }
  aside {
    position: absolute;
    top: 12px;
    right: 12px;
    bottom: 12px;
    width: 320px;
    overflow-y: auto;
    background: rgba(255, 255, 255, 0.95);
    border-radius: 10px;
    box-shadow: 0 2px 12px rgba(0, 0, 0, 0.15);
    padding: 12px 16px;
    box-sizing: border-box;
  }
  header {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 8px;
  }
  h1 {
    font-size: 18px;
    margin: 0;
  }
  h2 {
    font-size: 13px;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    color: #59636e;
    margin: 16px 0 8px;
  }
  .status {
    font-size: 12px;
  }
  .status.connected {
    color: #1a7f37;
  }
  .status.error {
    color: #d1242f;
  }
  .buttons {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
  }
  button {
    font: inherit;
    font-size: 13px;
    padding: 5px 10px;
    border-radius: 6px;
    border: 1px solid #d1d9e0;
    background: #f6f8fa;
    cursor: pointer;
  }
  button:disabled {
    opacity: 0.5;
    cursor: default;
  }
  .result {
    font-size: 12px;
    color: #59636e;
    min-height: 1em;
    word-break: break-word;
  }
  .vehicle {
    border: 1px solid #d1d9e0;
    border-radius: 8px;
    padding: 8px 10px;
    margin-bottom: 8px;
  }
  .title {
    display: flex;
    gap: 8px;
    align-items: baseline;
  }
  .pill {
    margin-left: auto;
    font-size: 11px;
    padding: 0 6px;
    border-radius: 8px;
  }
  .pill.ok {
    background: #dafbe1;
    color: #1a7f37;
  }
  .pill.bad {
    background: #ffebe9;
    color: #d1242f;
  }
  dl {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: 2px 12px;
    font-size: 13px;
    margin: 8px 0 0;
  }
  dt {
    color: #59636e;
  }
  dd {
    margin: 0;
    font-variant-numeric: tabular-nums;
  }
  .dim {
    color: #59636e;
    font-size: 12px;
  }
  .problems {
    font-size: 12px;
    color: #9a6700;
    margin: 6px 0 0;
    padding-left: 16px;
  }
</style>
