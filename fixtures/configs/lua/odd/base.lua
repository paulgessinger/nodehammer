-- ODD base: drop-all rule that goes before any subsystem keep rules.
--
-- Include order carries the meaning here. This fragment runs first and drops
-- everything; each subsystem fragment then keeps back what it owns. Without it
-- the selection inverts -- every node survives unless something drops it.

drop { 'name ~= "*"' }
