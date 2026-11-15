# External comparison camera

Set `WOWEE_VIEW_SYNC` to an absolute request-file path before launching WoWee.
Without this variable there is no file polling or camera override.

Write a complete request to a sibling temporary file and atomically rename it:

```text
1 request123 hold -6294.973145 275.276642 387.719666 -0.531125307 -0.780441582 -0.329874039 -0.185593024 -0.272712499 0.944024980 44.1235123
1 request124 release
```

Send one line per request. `hold` fields are render-space position XYZ,
normalized forward XYZ, normalized up XYZ and vertical FOV in degrees. Forward
and up must be perpendicular. Tokens contain lowercase ASCII letters/digits,
up to 64 characters. Version, payload and pose are validated. Errors are logged
and reported through `<path>.ack`; a rejected pose releases the override.

The camera applies after normal controller updates and before world visibility
queries. It remains held until release. `<path>.ack` is replaced atomically on
success, containing token, `OK`, mode, applied XYZ, vertical FOV, forward XYZ and
up XYZ. `ERROR` responses contain the reason. Normal controller movement still
runs while held. Release restores controller camera and the previous FOV.

Workspace `tools/client-loop/sync-view.py` drives this protocol alongside a
console-only server alignment command and read-only native camera supervisor.
It stores both screenshots and acknowledgement evidence outside this repository.
This interface does not teleport characters or alter gameplay by itself.

Same-map teleport handling must update both renderer character yaw and camera
controller facing from the server orientation. Updating movementInfo alone is
insufficient: per-frame movement synchronization copies renderer facing back
into outgoing packets. Live repeated server snapshots verify facing remains
aligned after the teleport acknowledgement, rather than only at acknowledgement.
