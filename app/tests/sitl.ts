import { execFileSync } from "node:child_process";
import { resolve } from "node:path";

export const repoRoot = resolve(import.meta.dirname, "../..");
export const fctl = process.env.FCTL ?? resolve(repoRoot, "build/cli/fctl");
const dataDir = resolve(repoRoot, ".falconer");

export function runFctl(...args: string[]): string {
  return execFileSync(fctl, ["--data-dir", dataDir, ...args], { encoding: "utf8" });
}

export function runningSitl(): string[] {
  return execFileSync("docker", ["ps", "-q", "--filter", "label=falconer.sitl=1"], {
    encoding: "utf8",
  })
    .split("\n")
    .filter(Boolean);
}
