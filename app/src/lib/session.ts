// Where the page gets its backend port and secrets. In browser development
// mode the Vite dev server provides them; the Tauri shell will provide them
// in the packaged app.

export interface Session {
  port: number;
  clientSecret: Uint8Array;
  serverSecret: Uint8Array;
  version: string;
}

function fromHex(hex: string): Uint8Array {
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  }
  return out;
}

export async function fetchSession(): Promise<Session> {
  const res = await fetch("/__falconer/session", { cache: "no-store" });
  const body = await res.json();
  if (!res.ok) {
    throw new Error(body.error ?? `session unavailable (${res.status})`);
  }
  return {
    port: body.port,
    clientSecret: fromHex(body.clientSecret),
    serverSecret: fromHex(body.serverSecret),
    version: body.version,
  };
}
