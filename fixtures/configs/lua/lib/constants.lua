-- Shared constants imported via use(). A pure module: it returns a table and
-- never calls the DSL, so it evaluates exactly once (cached) and is handed back
-- deep-frozen — a stray write raises instead of corrupting other importers.
return {
  endcaps = { "EndcapN", "EndcapP" },
  seg     = { coarse = 10, fine = 48 },
  palette = {
    silicon = "#60666E",
    kapton  = "#9A5516",
    copper  = "#B66A3C",
  },
}
