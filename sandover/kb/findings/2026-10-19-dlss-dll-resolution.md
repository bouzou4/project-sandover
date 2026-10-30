# DLSS DLL resolution: ship nvngx_dlss.dll or rely on the driver?

Date: 2026-10-18. Labels: [V] verified (URL given, or read on the test machine), [I] inferred, [E] external/forum claim, not verified against NVIDIA docs.

## Answer

Ship it. The driver supplies only the NGX core (_nvngx.dll / nvngx.dll); the feature DLL (nvngx_dlss.dll) is an app-side component. A driver-side copy exists only as an OTA/override cache, keyed by NVIDIA-known application ids, so it cannot be relied on for a private mod. [V]+[I]

## 1. Resolution order

- NGX core is a system component always installed with the driver. [V] https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf (section 1, "NGX Core Runtime")
- The deployment checklist requires the production nvngx_dlss.dll to be "packaged in the release build", next to the game executable (or plugin DLL). [V] same PDF, sections 2.5 and 4.2
- Alternate location: NVSDK_NGX_FeatureCommonInfo.PathListInfo (list of wide-char paths) passed to the Init call; NGX scans these for the feature DLL. [V] same PDF sections 2.3, 4.2; https://raw.githubusercontent.com/NVIDIA/DLSS/main/include/nvsdk_ngx_defs.h (struct NVSDK_NGX_PathListInfo)
- Signature: the DLL must keep NVIDIA's signature; NGX Core refuses a missing or corrupted one. [V] PDF section 4.2.2
- Model store: the NGX update module downloads newer feature versions and "swaps the DLL for the calling application". [V] PDF section 1 item 3. Location on disk: $ProgramData\NVIDIA\NGX\models\<feature>\versions\<n>\files\ [V] test machine, section 4.
- Not documented in the public SDK: exact precedence between PathList/app directory and the model store, and whether an app with no DLL anywhere is served from the store. The docs only describe shipping the DLL; NVSDK_NGX_Result_FAIL_FeatureNotFound is the failure code. [V] PDF section 8. Serving a DLL-less app: [I] unlikely, since store entries are per application id (section 4).
- The driver package contains no nvngx_dlss.dll (section 4). [V] test machine.
- Forum/reverse-engineering claim: nvngx.dll reads $ProgramData\NVIDIA\NGX\models\nvngx_config.txt and may load a different DLSS DLL than the one in the game folder. [E] https://github.com/emoose/dlsstweaks/issues/139

## 2. NVIDIA App "DLSS override"

- Official: NVIDIA App 11.0.2+ / driver 566.36+ adds per-game and global overrides (Transformer model upgrade for SR/RR/DLAA, MFG, presets), effective "when the features are ON in-game", i.e. only for apps that already use DLSS. [V, from search summaries, pages not read in full] https://www.nvidia.com/en-us/geforce/news/nvidia-app-update-dlss-overrides-and-more/ and https://www.nvidia.com/en-us/geforce/news/nvidia-app-update-g-assist-new-dlss-override-and-more/
- Allowlist: NVIDIA lists games with verified override support (RTX Games and Apps page); the app shows the toggle only for those. [V] same news pages. Forcing the toggle for unlisted games by editing the app's ApplicationStorage.json: [E] https://forums.guru3d.com/goto/post?id=6306145 (not retrievable, HTTP 403; search snippet only).
- Mechanism: [E]/[I] the driver redirects NGX's load to a driver-managed copy in the model store rather than rewriting the game file. Consistent with the test machine (dlss_override store plus [dlss_override] in nvngx_config.txt). Forum sources conflict on whether game files are touched.
- Forcing for unlisted apps [E]: NVIDIA Profile Inspector, group "5 - Common": "DLSS - Enable DLL Override" = "On - DLSS overridden by latest available"; optionally "DLSS - Forced Preset Letter". Third-party nvidiaDlssGlom has "Force Enable DLSS Override on All DLSS Titles". Sources (not retrievable, HTTP 403; search snippets only): https://forum.dcs.world/topic/368685-dlss-4-global-override-easiest-method-without-nvidia-bloatware/ , https://forums.guru3d.com/threads/nvidiadlssglom-manually-update-the-latest-dlss-models-without-nvidia-app.457047/ . DRS setting ids from DLSSTweaks (0x10E41DF3 preset selection, 0x10E41DF4 force DLAA, 0x10E41DF5 scaling ratio) and the note that they must sit on the global/base profile: https://github.com/emoose/dlsstweaks/issues/139 [E].
- Consequence [I]: the override upgrades an app that already loads some DLSS; it is a user-side opt-in, not a dependency a mod can rely on.

## 3. OTA snippet updates

- Mechanism: the app calls NVSDK_NGX_UpdateFeature(ApplicationId, NVSDK_NGX_Feature_SuperSampling) on a dedicated thread; the update applies at the next NVSDK_NGX_<API>_Init. [V] PDF section 2.4
- Requirement: a valid NVSDK_NGX_Application_Identifier: NVIDIA-issued ApplicationId, or ProjectDesc (project GUID + engine type; CUSTOM allowed). "Required for over the air updates (both optional and mandatory)." [V] nvsdk_ngx_defs.h comment above NVSDK_NGX_Application_Identifier; PDF section 2.3
- Still present in the guide dated 31 March 2026 (310.x era). [V] PDF section 2.4. There is no separate "allow OTA" flag; calling UpdateFeature is the opt-in. Titles that opt in and use the default preset may have the default model changed. [V] PDF section 3.12.1
- Downloads land under $ProgramData\NVIDIA\NGX\models\<feature>\versions\<n>\files\. [V] test machine
- OTA updates an existing integration; the guide still requires shipping the DLL (checklist 2.5). [V]

## 4. The test machine (read-only)

- A current NVIDIA driver on an RTX-class GPU. [V]
- Driver store (the display driver's `FileRepository` folder): _nvngx.dll, nvngx.dll and nvngx_dlssg.dll (frame generation, 310.2.1.0). No nvngx_dlss.dll, no nvngx_dlssd.dll. [V]
- The system folder: no nvngx*.dll. [V]
- NVIDIA App install folder: only NvDLISR\nvngx_dlisr.dll 1.2.0.0 (DLISR, not DLSS). [V]
- RTX Remix bundles its own nvngx_dlss.dll 310.1.0.0 and others: NVIDIA's own app ships the DLL. [V]
- Model store $ProgramData\NVIDIA\NGX\models: folders config, dlisp, dlss, dlss_override, dlssd, dlssg, sl_* (Streamline plugins and *_override_0), plus nvngx_config.txt. [V]
  - dlss\versions\<n>\files\160_<id>.bin: DLSS DLLs stored as .bin, from 1.0.108 up to 310.2.1, 310.3.0, 310.5.0, 310.5.2, 310.6.0, 310.7.129, 310.9.0, 310.9.16. [V]
  - dlss_override\versions\<n>\files\<app id>\ : nvngx_package_config.txt (maps the dlss 310.9.16 .bin to nvngx_dlss.dll), sl.*.dll; a plain nvngx_dlss.dll is present in the oldest version folder only. [V]
  - nvngx_config.txt: [dlss] and [dlss_override] entries per application id (310.9.16 for the current one), [dlssd]/[dlssg] 310.9.16, plus many legacy per-app ids (1.0.108 to 2.3.4). [V]
  - config\versions\1\files\nvngx_mapping.json maps engine types (ue4, unity, omniverse, custom) and projectIDs to cmsIDs (one generic id for custom engines, no project mappings); nvngx_deny_list.txt holds a [streamline-ota] entry; config\versions\2\files\nvngx_server_config.txt is the server-side version map. [V]
- Reading [I]: the store is populated for NVIDIA-known application ids. An unknown project id has no mapping, so a DLL-less private app would not be served from it automatically. Not tested at the time of this note.

## 5. Licence

- https://raw.githubusercontent.com/NVIDIA/DLSS/main/LICENSE.txt (RTX SDK licence plus RTX supplement) [V]: grants the right to distribute SDK materials "as incorporated in object code format into a software application" (1c); no stand-alone redistribution (3b); terms at least as protective (2c); notify NVIDIA before commercial release (supplement 4); NVIDIA marks on splash/credits; DLSS licensed only for NVIDIA GPU systems (supplement 1).
- Nothing in the licence or supplement addresses relying on a driver-supplied DLL instead of shipping one, or the user-supplied-DLL case (checked via keyword scan of the clauses, not a full legal read). [V]/[I]
- The guide assumes the app ships the DLL (2.5, 4.2) and requires a third-party notice (4.2.3, text in 9.5); the dev (watermarked) DLL must never be distributed. [V]
- User-supplied DLL: no explicit permission or prohibition found. [I] Having users download it moves distribution to them but is untested legally; confirm with nvidia-rtx-license-questions@nvidia.com if chosen.

## Recommendation

Ship the release nvngx_dlss.dll (lib/Windows_x86_64/rel) beside the executable or point PathListInfo at the mod folder; optionally call NVSDK_NGX_UpdateFeature with a CUSTOM-engine ProjectDesc. Treat NVIDIA App override as a user-side bonus, never a dependency.

Outcome (`2026-10-18-dlss.md`): the licence reading there led to not shipping the DLL. The engine
uses `nvngx_dlss.dll` beside `gk` first, else points PathListInfo at the newest plain
`nvngx_dlss.dll` in the NVIDIA App's override cache, else reports DLSS unavailable and runs
FSR 3.1; the second case was tested and creates the feature.
