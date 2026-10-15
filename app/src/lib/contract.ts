// Contract client: one authenticated WebSocket to the backend.
// Handshake as documented in proto/falconer/v1/contract.proto.

import { create, fromBinary, toBinary, type MessageInitShape } from "@bufbuild/protobuf";
import {
  ClientMessageSchema,
  ErrorCode,
  RequestSchema,
  ServerMessageSchema,
  type Response,
  type ServerMessage,
  type StreamBatch,
} from "../gen/falconer/v1/contract_pb";
import type { Session } from "./session";

const NONCE_BYTES = 32;
const REQUEST_TIMEOUT_MS = 15000;

async function hmac(key: Uint8Array, data: Uint8Array): Promise<Uint8Array> {
  const cryptoKey = await crypto.subtle.importKey(
    "raw",
    key as BufferSource,
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"],
  );
  return new Uint8Array(await crypto.subtle.sign("HMAC", cryptoKey, data as BufferSource));
}

function equal(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
  return diff === 0;
}

export function errorText(code: ErrorCode, message: string): string {
  return `${ErrorCode[code] ?? code}: ${message}`;
}

type StreamListener = (batch: StreamBatch) => void;
export type Command = MessageInitShape<typeof RequestSchema>["cmd"];
type ClientMsg = MessageInitShape<typeof ClientMessageSchema>["msg"];

export class ContractClient {
  private ws?: WebSocket;
  private nextId = 1;
  private pending = new Map<
    number,
    { resolve: (r: Response) => void; reject: (e: Error) => void; timer: number }
  >();
  private listeners = new Set<StreamListener>();
  onClose?: (reason: string) => void;

  onStream(listener: StreamListener): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  async connect(session: Session): Promise<void> {
    const ws = new WebSocket(`ws://127.0.0.1:${session.port}/`);
    ws.binaryType = "arraybuffer";
    this.ws = ws;

    const queue: ServerMessage[] = [];
    let wake: (() => void) | undefined;
    let closed: string | undefined;
    const next = async (): Promise<ServerMessage> => {
      while (queue.length === 0) {
        if (closed) throw new Error(closed);
        await new Promise<void>((r) => (wake = r));
      }
      const msg = queue.shift()!;
      if (msg.msg.case === "fatal") {
        throw new Error(
          "backend refused the session: " + errorText(msg.msg.value.code, msg.msg.value.message),
        );
      }
      return msg;
    };
    ws.onmessage = (ev) => {
      queue.push(fromBinary(ServerMessageSchema, new Uint8Array(ev.data as ArrayBuffer)));
      wake?.();
    };
    ws.onclose = (ev) => {
      closed = `connection closed (${ev.code})`;
      wake?.();
    };

    await new Promise<void>((resolve, reject) => {
      ws.onopen = () => resolve();
      ws.onerror = () => reject(new Error(`cannot reach the backend on port ${session.port}`));
    });

    const clientNonce = crypto.getRandomValues(new Uint8Array(NONCE_BYTES));
    this.sendRaw({ case: "hello", value: { version: session.version, clientNonce } });
    const ack = await next();
    if (ack.msg.case !== "helloAck") throw new Error("expected hello_ack");
    const expected = await hmac(session.serverSecret, clientNonce);
    if (!equal(ack.msg.value.serverProof, expected)) {
      ws.close();
      throw new Error("backend failed to prove its identity; refusing to talk to it");
    }
    const clientProof = await hmac(session.clientSecret, ack.msg.value.serverNonce);
    this.sendRaw({ case: "auth", value: { clientProof } });
    const ok = await next();
    if (ok.msg.case !== "authOk") throw new Error("expected auth_ok");

    // Authenticated: switch to normal dispatch.
    ws.onmessage = (ev) => this.dispatch(new Uint8Array(ev.data as ArrayBuffer));
    ws.onclose = (ev) => {
      for (const p of this.pending.values()) {
        clearTimeout(p.timer);
        p.reject(new Error("connection lost"));
      }
      this.pending.clear();
      this.onClose?.(`connection closed (${ev.code})`);
    };
  }

  request(cmd: Command): Promise<Response> {
    const requestId = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = window.setTimeout(() => {
        this.pending.delete(requestId);
        reject(new Error("request timed out"));
      }, REQUEST_TIMEOUT_MS);
      this.pending.set(requestId, { resolve, reject, timer });
      this.sendRaw({ case: "request", value: { requestId, cmd } });
    });
  }

  private sendRaw(msg: ClientMsg) {
    const message = create(ClientMessageSchema, { msg });
    this.ws!.send(toBinary(ClientMessageSchema, message));
  }

  private dispatch(bytes: Uint8Array) {
    const msg = fromBinary(ServerMessageSchema, bytes);
    switch (msg.msg.case) {
      case "response": {
        const p = this.pending.get(msg.msg.value.requestId);
        if (p) {
          clearTimeout(p.timer);
          this.pending.delete(msg.msg.value.requestId);
          p.resolve(msg.msg.value);
        }
        break;
      }
      case "stream":
        for (const listener of this.listeners) listener(msg.msg.value);
        break;
      case "fatal":
        this.onClose?.(
          "backend closed the session: " + errorText(msg.msg.value.code, msg.msg.value.message),
        );
        break;
    }
  }
}
