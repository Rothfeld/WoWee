# Gathering animation and loot

Run `build/bin/test_input_integration '[mining-loot]'` from the repository root.

Mining uses the spell's SpellVisual precast kit animation. TBC Mining 2575
references SpellVisual 93, precast kit 166, animation 62 (EmoteWork).
The tool attachment is independent of the character animation.

OPEN_LOCK effects open loot on the server. Gathering, lock-opening spells,
and key-item spells must not schedule CMSG_LOOT retries or send CMSG_LOOT
after SPELL_GO. Failed skill rolls do not produce loot; retrying the click
casts again. Successful casts produce SMSG_LOOT_RESPONSE.

Parse loot responses into a temporary value before publishing them. A rejected
open response must not erase items already displayed in an existing loot window.
Lua visible slot indices are separate from the server's loot slot indices.

TBC client loot type 2 covers pickpocketing and gathering. Fishing is type 3.
IsFishingLoot must use the client wire type, not the server's internal enum.
