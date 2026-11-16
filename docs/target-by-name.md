# Target commands

Run `build/bin/test_input_integration '[target-name]'` from the repository root.

TBC `interface/framexml/chatframe.lua` implements `/target` with
`TargetUnit(name)` and `/targetexact` with `TargetUnit(name, 1)`.
The binding accepts unit tokens and case-insensitive nearby unit names.
Partial names match the start of the name; exact mode requires the whole name.
When several visible units match, the nearest is selected. Unmatched names
leave the current target unchanged.

Keep name resolution in the targeting operation. Unit-query APIs use unit
tokens, and broadening their shared resolver changes unrelated UI behavior.
