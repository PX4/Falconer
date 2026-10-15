# Missions

Example missions in QGroundControl `.plan` format, used by the integration tests and for trying Falconer against PX4 SITL.

| File | Source |
| --- | --- |
| `px4-multicopter-example.plan` | PX4-Autopilot `test/mavsdk_tests/multicopter_mission_geofence_avoid.plan` (commit `40e61d24298`), BSD-3-Clause. Takeoff to 20 m, three waypoints, RTL, around the default SIH home position. |

Only mission items are uploaded; geofence and rally points in a plan are ignored for now.
