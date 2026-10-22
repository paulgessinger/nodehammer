return function()
  -- Visual PBR finishes adapted from ODD and the supplied ATLAS installation photos.
  -- Prefer satin/aged metals and matte panel faces over polished surfaces.
  -- Composite/homogenized source materials use representative appearances,
  -- not measured optical constants. Selection and geometry are unchanged.
  local function finish(name, props, patterns)
    material(name, props)
    if patterns then
      local matches = {}
      for _, pattern in ipairs(patterns) do
        matches[#matches+1] = ('material ~= "%s"'):format(pattern)
      end
      rule { match = table.concat(matches, " || "), material = name }
    end
  end

  finish("atlas_neutral", { base_color = "#858580", metallic = 0, roughness = 0.7 })
  rule { match = "true", material = "atlas_neutral" } -- explicit fallback for unclassified mixtures
  finish("atlas_aluminum", { base_color = "#C8C8C8", metallic = 1, roughness = 0.65 },
    {"std::Aluminium", "Aluminium*"})
  finish("atlas_steel", { base_color = "#7F8487", metallic = 1, roughness = 0.72 },
    {"std::Iron", "std::SSteel", "std::Inconel*", "shield::ShieldIron", "shield::ShieldSteel"})
  finish("atlas_beryllium", { base_color = "#A8A8A0", metallic = 0.8, roughness = 0.55 }, {"std::Beryllium"})
  finish("atlas_titanium", { base_color = "#9A9DA3", metallic = 1, roughness = 0.65 }, {"std::Pump_Ti", "*Titanium*", "TiMetal*"})
  finish("atlas_copper", { base_color = "#B66A3C", metallic = 1, roughness = 0.65 }, {"std::Copper", "LAr::FCal1Absorber"})
  finish("atlas_tungsten", { base_color = "#808078", metallic = 1, roughness = 0.75 }, {"LAr::FCal23Absorber"})
  finish("atlas_brass", { base_color = "#B59A54", metallic = 1, roughness = 0.65 }, {"*Brass"})
  finish("atlas_lead", { base_color = "#777B85", metallic = 1, roughness = 0.8 }, {"std::Lead"})
  finish("atlas_silicon", { base_color = "#60666E", metallic = 0.35, roughness = 0.42 },
    {"std::Silicon", "Silicon*", "SiMetal*"})
  finish("atlas_carbon", {
    base_color = "#292A29", metallic = 0, roughness = 0.75,
    specular = 0.30, anisotropy = 0.15, clearcoat = 0.0,
  }, {"std::Carbon", "CFRP*", "CarbonFibre*", "SupportCarbon*", "K13*",
      "pix::DiskSector*", "pix::Omega*", "OmegaGlue_IBL", "FacePlateGlueGrease_IBL", "EndblockOg*", "sct::FwdSpine*",
      "pix::Halfshell*", "pix::AsRing*", "pix::StaveRing*", "pix::Prepreg",
      "sct::DiscSupport*", "sct::SupportCyl*", "sct::FwdSupport",
      "trt::*Support*", "trt::ModuleShell*", "InclL*Material", "InnerPixBarrelSupport_*Material"})
  finish("atlas_foam", { base_color = "#181818", metallic = 0, roughness = 0.92 }, {"T300CF*", "*CarbonFoam*"})
  finish("atlas_honeycomb", { base_color = "#B6AB8B", metallic = 0, roughness = 0.8 }, {"Honeycomb*", "muo::Honeycomb"})
  finish("atlas_g10", { base_color = "#B5A85A", metallic = 0, roughness = 0.65, specular = 0.30 },
    {"*G10", "*FR4", "sct::PCB", "LAr::LArElectronics"})
  finish("atlas_ceramic", { base_color = "#C6C3B5", metallic = 0, roughness = 0.75 },
    {"sct::AlNitride", "sct::BrlBaseBoard"})
  -- Opaque effective finishes: the retained straw is a homogenized outer
  -- cylinder, so transmitting through it would reveal omitted gas/wire internals.
  finish("atlas_polyimide", { base_color = "#9A5516", metallic = 0, roughness = 0.65, ior = 1.70, clearcoat = 0.0 },
    {"std::Kapton", "trt::Straw", "std::Vespel", "pix::BP_VespelRing2"})
  finish("atlas_polymer", { base_color = "#D7D4C8", metallic = 0, roughness = 0.65 },
    {"Peek*", "Torlon*", "std::Polyethylene", "shield::Polyboron*", "LAr::BoratedPolyethelyne"})
  finish("atlas_insulation", { base_color = "#DEDCD2", metallic = 0, roughness = 0.95 },
    {"std::Aerogel*", "std::Pyrogel*"})

  -- Broad brown chamber faces and muted coated housings seen in installation
  -- photos. These are effective assembly finishes, not gas-volume materials.
  finish("atlas_chamber_panel", { base_color = "#81633F", metallic = 0, roughness = 0.85, specular = 0.25 })
  finish("atlas_housing", { base_color = "#A4A6A2", metallic = 0.15, roughness = 0.8, specular = 0.3 })

  -- Retained detector envelopes represent assembled systems, not visible gas
  -- or liquid. Use opaque, neutral housing finishes instead of literal fluid optics.
  rule { match = 'path ~= "/WorldLog/Muon/*_station"', material = "atlas_chamber_panel" }
  rule { match = 'path ~= "/WorldLog/Muon/ANON/**" && ((volume ~= "sTGC*" && material == "muo::Honeycomb") || volume ~= "MM_1-*")', material = "atlas_chamber_panel" }
  rule { match = 'path ~= "/WorldLog/Tile/**"', material = "atlas_housing" }
  rule { match = 'path ~= "/WorldLog/LAr*/**" && material == "std::LiquidArgon"', material = "atlas_housing" }

end
