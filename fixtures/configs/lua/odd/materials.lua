-- ODD material definitions and source-material mapping rules.

-- ── Render material definitions ──────────────────────────────────────────────

material("beryllium", { base_color = "#A8A8A0", metallic = 0.8, roughness = 0.2 })

material("support", { base_color = { 0.5, 0.5, 0.5, 1.0 }, metallic = 0.0, roughness = 0.7 })

material("silicon", { base_color = "#60666E", metallic = 1.0, roughness = 0.05 })

material("kapton", {
  base_color = "#9A5516", metallic = 0.0, roughness = 0.16,
  ior = 1.70, transmission = 0.85, clearcoat = 0.1,
})

material("aluminum", { base_color = "#C8C8C8", metallic = 1.0, roughness = 0.12 })

material("carbon_fiber", {
  base_color = "#1A1A1A", metallic = 0.0, roughness = 0.32,
  specular = 0.60, anisotropy = 0.35,
  clearcoat = 0.20, clearcoat_roughness = 0.08,
})

material("titanium", { base_color = "#9A9DA3", metallic = 1.0, roughness = 0.28 })

material("carbon_foam", { base_color = { 0.08, 0.08, 0.08 }, metallic = 0.0, roughness = 0.92 })

material("copper", { base_color = "#B66A3C", metallic = 1.0, roughness = 0.18 })

material("tungsten_dens24", {
  base_color = "#808078", metallic = 1.0, roughness = 0.50,
  specular = 0.50, anisotropy = 0.0,
  clearcoat = 0.0, clearcoat_roughness = 0.0,
})

material("g10", { base_color = "#B5A85A", metallic = 0.0, roughness = 0.65, specular = 0.30 })

material("ground_or_hv_mix", {
  base_color = "#A0623A", metallic = 1.0, roughness = 0.55, specular = 0.55,
})

material("si_pcb_mix", {
  base_color = "#966050", metallic = 0.85, roughness = 0.60, specular = 0.50,
})

material("steel", { base_color = "#7F8487", metallic = 1.0, roughness = 0.35, specular = 0.50 })

material("polystyrene", {
  base_color = "#E8E4DA", metallic = 0.0, roughness = 0.50,
  ior = 1.59, transmission = 0.15, clearcoat = 0.05,
})

-- ── Source-material → render-material mapping rules ──────────────────────────
-- One rule per entry, in order. Two source materials are spelled inconsistently
-- upstream, so those carry an explicit `||` rather than a second rule -- keeping
-- one rule per render material.

local material_map = {
  { 'material ~= "Beryllium"',      "beryllium" },
  { 'material ~= "Silicon"',        "silicon" },
  { 'material ~= "Kapton"',         "kapton" },
  { 'material ~= "Aluminum"',       "aluminum" },
  { 'material ~= "CarbonFiber*"',   "carbon_fiber" },
  { 'material ~= "Titanium"',       "titanium" },
  { 'material ~= "CarbonFoam"',     "carbon_foam" },
  { 'material ~= "Copper"',         "copper" },
  { 'material ~= "TungstenDens24"', "tungsten_dens24" },
  { 'material ~= "G10"',            "g10" },
  { 'material ~= "GroundOrHvMix" || material ~= "GroundOrHVMix"', "ground_or_hv_mix" },
  { 'material ~= "SiPcbMix" || material ~= "siPCBMix"',           "si_pcb_mix" },
  { 'material ~= "Steel*"',         "steel" },
  { 'material ~= "Polystyrene"',    "polystyrene" },
}

for _, m in ipairs(material_map) do
  rule { match = m[1], material = m[2] }
end
