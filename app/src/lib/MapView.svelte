<script lang="ts">
  import * as maplibregl from "maplibre-gl";
  import "maplibre-gl/dist/maplibre-gl.css";
  import { onMount } from "svelte";
  import type { ContractClient } from "./contract";
  import type { Plan } from "./plan";

  let { client, plan }: { client: ContractClient; plan: Plan | undefined } = $props();

  let container: HTMLDivElement;
  let map: maplibregl.Map | undefined;
  let loaded = $state(false);

  // OpenStreetMap raster tiles; fine for development. The map adapter will
  // make the engine and tile source swappable.
  const style: maplibregl.StyleSpecification = {
    version: 8,
    sources: {
      osm: {
        type: "raster",
        tiles: ["https://tile.openstreetmap.org/{z}/{x}/{y}.png"],
        tileSize: 256,
        maxzoom: 19,
        attribution: "© OpenStreetMap contributors",
      },
    },
    layers: [{ id: "osm", type: "raster", source: "osm" }],
  };

  // Default view: PX4 SIH home position.
  const DEFAULT_CENTER: [number, number] = [8.545594, 47.397742];

  interface MarkerEntry {
    marker: maplibregl.Marker;
    el: HTMLDivElement;
    arrow: HTMLDivElement;
  }
  const markers = new Map<string, MarkerEntry>();

  function markerFor(id: string): MarkerEntry {
    let entry = markers.get(id);
    if (entry) return entry;
    const el = document.createElement("div");
    el.className = "vehicle-marker";
    el.setAttribute("data-testid", `marker-${id}`);
    const arrow = document.createElement("div");
    arrow.className = "vehicle-arrow";
    arrow.innerHTML =
      '<svg viewBox="0 0 24 24" width="28" height="28"><path d="M12 2 L20 21 L12 16 L4 21 Z"/></svg>';
    const label = document.createElement("div");
    label.className = "vehicle-label";
    label.textContent = id;
    el.append(arrow, label);
    const marker = new maplibregl.Marker({ element: el }).setLngLat(DEFAULT_CENTER).addTo(map!);
    entry = { marker, el, arrow };
    markers.set(id, entry);
    return entry;
  }

  onMount(() => {
    map = new maplibregl.Map({ container, style, center: DEFAULT_CENTER, zoom: 15 });
    map.addControl(new maplibregl.NavigationControl(), "top-left");
    map.on("load", () => {
      map!.addSource("route", { type: "geojson", data: emptyRoute() });
      map!.addLayer({
        id: "route-line",
        type: "line",
        source: "route",
        filter: ["==", "$type", "LineString"],
        paint: { "line-color": "#2f74d0", "line-width": 3, "line-dasharray": [2, 1] },
      });
      map!.addLayer({
        id: "route-points",
        type: "circle",
        source: "route",
        filter: ["==", "$type", "Point"],
        paint: {
          "circle-radius": 7,
          "circle-color": "#ffffff",
          "circle-stroke-color": "#2f74d0",
          "circle-stroke-width": 2.5,
        },
      });
      loaded = true;
    });

    // Positions go straight to the map engine, not through reactive state.
    const unsubscribe = client.onStream((batch) => {
      if (!map) return;
      for (const s of batch.vehicles) {
        const pos = s.position;
        if (!pos || (pos.latitudeDeg === 0 && pos.longitudeDeg === 0)) continue;
        const entry = markerFor(s.id);
        entry.marker.setLngLat([pos.longitudeDeg, pos.latitudeDeg]);
        entry.arrow.style.transform = `rotate(${s.attitude?.yawDeg ?? 0}deg)`;
        entry.el.dataset.lat = pos.latitudeDeg.toFixed(7);
        entry.el.dataset.lon = pos.longitudeDeg.toFixed(7);
        entry.el.classList.toggle("armed", s.armed);
        entry.el.classList.toggle("lost", !s.connected);
      }
    });

    return () => {
      unsubscribe();
      map?.remove();
    };
  });

  function emptyRoute() {
    return { type: "FeatureCollection" as const, features: [] };
  }

  // Draw the mission route whenever the plan changes.
  $effect(() => {
    if (!loaded || !map) return;
    const source = map.getSource("route") as maplibregl.GeoJSONSource;
    if (!plan || plan.waypoints.length === 0) {
      source.setData(emptyRoute());
      container.dataset.routeFeatures = "0";
      return;
    }
    const coords = plan.waypoints.map((w) => [w.lon, w.lat] as [number, number]);
    source.setData({
      type: "FeatureCollection",
      features: [
        { type: "Feature", properties: {}, geometry: { type: "LineString", coordinates: coords } },
        ...coords.map((c, i) => ({
          type: "Feature" as const,
          properties: { seq: plan!.waypoints[i].seq },
          geometry: { type: "Point" as const, coordinates: c },
        })),
      ],
    });
    const bounds = coords.reduce(
      (b, c) => b.extend(c),
      new maplibregl.LngLatBounds(coords[0], coords[0]),
    );
    // Keep the route clear of the side panel (320 px plus margins).
    map.fitBounds(bounds, {
      padding: { top: 60, bottom: 60, left: 60, right: 400 },
      maxZoom: 17,
      duration: 0,
    });

    // Record what the map engine actually loaded, so tests can tell a drawn
    // route from one that was only handed over.
    const m = map;
    m.once("idle", () => {
      container.dataset.routeFeatures = String(m.querySourceFeatures("route").length);
    });
  });
</script>

<div class="map" bind:this={container} data-testid="map"></div>

<style>
  .map {
    position: absolute;
    inset: 0;
  }
  :global(.vehicle-marker) {
    display: flex;
    flex-direction: column;
    align-items: center;
    pointer-events: none;
  }
  :global(.vehicle-arrow svg path) {
    fill: #e5612b;
    stroke: #ffffff;
    stroke-width: 1.5;
  }
  :global(.vehicle-marker.armed .vehicle-arrow svg path) {
    fill: #d1242f;
  }
  :global(.vehicle-marker.lost) {
    opacity: 0.4;
  }
  :global(.vehicle-label) {
    font: 600 11px system-ui, sans-serif;
    color: #1f2328;
    background: rgba(255, 255, 255, 0.85);
    border-radius: 4px;
    padding: 0 4px;
    margin-top: 2px;
  }
</style>
