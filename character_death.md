# Character Death Behavior

This project does not have a health system or a game-over screen. Instead, the "death" condition is treated as an immediate reset when the player collides with an obstacle.

## What Triggers Death

- Each frame, the player AABB is tested against every obstacle AABB.
- If any overlap is found, the run is considered a hit.

See the collision test and hit flag in [main.c](main.c#L2472-L2493) and the AABB helper in [main.c](main.c#L1807-L1816).

## What Happens On Hit

When a hit is detected, the game immediately resets the run state:

- Player position is reset to the origin on X/Z.
- Score distance is reset to 0.
- Player animation time is reset.
- All obstacles are respawned ahead of the player.

This logic is in [main.c](main.c#L2495-L2504).

## Notes

- There is no explicit "dead" state; the run simply restarts on collision.
- Camera and movement continue normally after the reset.
