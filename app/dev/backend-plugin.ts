// Browser development mode: plays the role the Tauri shell has in the
// packaged app. When the Vite dev server starts, it launches
// falconer-backend with fresh secrets on stdin, reads the contract port from
// stdout, and serves both to the page at /__falconer/session. Dev server
// only; the packaged app gets them from the Tauri shell instead.

import { spawn, type ChildProcess } from "node:child_process";
import { randomBytes } from "node:crypto";
import { createWriteStream, mkdirSync, readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import type { Plugin } from "vite";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "../..");

export interface SessionInfo {
  port: number;
  clientSecret: string; // hex
  serverSecret: string; // hex
  version: string;
}

export function falconerBackend(options: { origin: string }): Plugin {
  let child: ChildProcess | undefined;
  let session: SessionInfo | undefined;
  let failure: string | undefined;

  return {
    name: "falconer-backend",
    apply: "serve",
    configureServer(server) {
      const binary =
        process.env.FALCONER_BACKEND ?? resolve(repoRoot, "build/backend/falconer-backend");
      const version = readFileSync(resolve(repoRoot, "VERSION"), "utf8").trim();
      const logDir = resolve(repoRoot, ".falconer/logs");
      mkdirSync(logDir, { recursive: true });
      const logPath = resolve(logDir, `backend-app-${Date.now()}.log`);
      const log = createWriteStream(logPath);

      const clientSecret = randomBytes(32).toString("hex");
      const serverSecret = randomBytes(32).toString("hex");
      child = spawn(binary, ["--allow-origin", options.origin], {
        stdio: ["pipe", "pipe", "pipe"],
      });
      child.stderr!.pipe(log);
      child.stdin!.write(`${clientSecret} ${serverSecret}\n`);
      // stdin stays open: the backend exits when it closes.

      let out = "";
      child.stdout!.on("data", (chunk: Buffer) => {
        out += chunk.toString();
        const match = /FALCONER_LISTENING port=(\d+)/.exec(out);
        if (match && !session) {
          session = { port: Number(match[1]), clientSecret, serverSecret, version };
          server.config.logger.info(
            `  falconer-backend on 127.0.0.1:${session.port}, log ${logPath}`,
          );
        }
      });
      child.on("error", (err) => {
        failure = `cannot start ${binary}: ${err.message}`;
        server.config.logger.error(failure);
      });
      child.on("exit", (code) => {
        if (!session || code !== 0) {
          failure = `falconer-backend exited (code ${code}); see ${logPath}`;
          server.config.logger.error(failure);
        }
        session = undefined;
      });

      const stop = () => {
        child?.stdin?.end();
        child = undefined;
      };
      server.httpServer?.once("close", stop);
      process.once("exit", stop);

      server.middlewares.use("/__falconer/session", (_req, res) => {
        res.setHeader("Content-Type", "application/json");
        res.setHeader("Cache-Control", "no-store");
        if (session) {
          res.end(JSON.stringify(session));
        } else {
          res.statusCode = 503;
          res.end(JSON.stringify({ error: failure ?? "backend starting" }));
        }
      });
    },
  };
}
