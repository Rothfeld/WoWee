# Collision replay data

`WOWEE_DUMP_M2_COLLISION=/absolute/path/to/empty-directory` exports authored M2
collision meshes encountered by floor queries. Start the client with this
environment variable and walk through the affected scene. The directory must
already exist; existing destination files are rejected.

Each OBJ filename identifies its placed instance. Its first comment names the
source model. Vertices use world coordinates after the actual instance matrix;
faces use the original collision indices. Nine significant digits preserve
single-precision positions. No render mesh or invented collision is exported.
Output reports failures and stops rather than leaving a successful-looking dump.

Keep exports with the experiment's screenshots and movement traces. These files
contain original game asset data and belong outside the source repository.
M2 exports do not include WMO floors or terrain; document any approximated floor
used in an offline replay. A table-only replay omits nearby hammer collisions,
which can materially change the character's supported height.
