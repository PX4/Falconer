import { runFctl, runningSitl } from "./sitl";

// Every run starts from fresh SITL vehicles. Containers already running may
// belong to someone else's session, so they are only replaced on request.
export default function globalSetup() {
  if (runningSitl().length > 0) {
    if (process.env.FALCONER_TEST_RESTART_SITL !== "1") {
      throw new Error(
        "PX4 SITL containers are already running (fctl sitl ps). Stop them with " +
          "`fctl sitl down`, or set FALCONER_TEST_RESTART_SITL=1 to let the test replace them.",
      );
    }
    runFctl("sitl", "down");
  }
  process.stdout.write(runFctl("sitl", "up", "1"));
}
