import { runFctl } from "./sitl";

export default function globalTeardown() {
  if (process.env.FALCONER_TEST_KEEP_SITL === "1") return;
  process.stdout.write(runFctl("sitl", "down"));
}
