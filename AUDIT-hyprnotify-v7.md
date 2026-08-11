# hyprnotify v7.0.1 UI/UX Audit vs. Pixel/AOSP

**Date:** 2026-08-11  
**Baseline:** Tag `hyprnotify/v6.9.2` (pre-Codex rewrite)  
**Current:** Commit `289a2e7` (v7.0.1, post-rewrite at `47ec074`)  
**Reference:** Pixel 7a ROM CP2A.260705.006 + AOSP Material 3 specification  

---

## Executive Summary

The Codex rewrite (commit `47ec074 hyprnotify: implement Pixel notification model`) improved the functional model (conversation ranking, proper message handling, structured grouping) but **regressed the visual identity**:

### Key Issues Found

1. **❌ Color palette**: Lost the distinctive **glass·ink** material (graphite glass #0f1218 @ 62%, heritage cyan #32d6ff) in favor of generic blue/teal Pixel colors
2. **❌ Typography**: Changed from **IBM Plex Sans** to Roboto
3. **❌ Card radius**: Changed from **16px** to **24px** (AOSP uses **12dp**)
4. **❌ Rounding power**: Changed from **3.0** to **2.0** (correct for Material 3, but loses the heritage superellipse)
5. **❌ Bundling threshold**: Set to **2** instead of AOSP's **4** (`center.cpp:27`)
6. **❌ Shade radius**: `PIXEL_SHADE_RADIUS = 32` (should be **12** to match AOSP card radius)
7. **❌ Internal radius**: `PIXEL_INTERNAL_RADIUS = 4` (should be **12**)
8. **❌ Timed mutes**: **Removed** — `policy.cpp` now only writes persistent silence (expiry=0), no UI affordance for time-bounded mutes despite the 6.9.0 feature being documented in memory

### What Works

- ✅ Functional model: conversation ranking, message ordering, priority conversations
- ✅ Semantic role expansion: SURFACE, STATE, ON_PRIMARY, ON_ERROR config schema
- ✅ OSD band isolation (9990-9999)
- ✅ Declared groups vs. automatic groups logic
- ✅ Animation timing (320ms spatial motion is within AOSP's 300-400ms range)

---

## 1. Color Palette Restoration

### 1.1 Old Palette (v6.9.2 — **RESTORE THIS**)

From `common/theme.hpp` at tag `hyprnotify/v6.9.2`:

```cpp
namespace NHyprCommon::Theme {
    // glass·ink material — graphite frosted glass + heritage cyan
    inline constexpr uint64_t GLASS      = 0x9e0f1218; // 62% graphite (#0f1218)
    inline constexpr uint64_t INK        = 0xffe4e8ee; // primary text
    inline constexpr uint64_t TITLE      = 0xffeef1f5; // emphasis
    inline constexpr uint64_t SUB        = 0xff98a2ac; // secondary text
    inline constexpr uint64_t ACCENT     = 0xff32d6ff; // heritage cyan ⭐
    inline constexpr uint64_t ACCENT_DIM = 0x2932d6ff; // @16% state layer
    inline constexpr uint64_t ON_ACCENT  = 0xff07161c; // text on cyan fill
    inline constexpr uint64_t URGENT     = 0xffff8a5c; // critical/urgent
    inline constexpr uint64_t LINK       = 0xff7db4ff; // body hyperlinks
    inline constexpr uint64_t FILL       = 0x0bffffff; // white @4.5% resting
    inline constexpr uint64_t FILL2      = 0x17ffffff; // white @9% raised
    inline constexpr uint64_t LINE       = 0x17dcebff; // hairlines @9%
    inline constexpr uint64_t SHADOW     = 0x73000000; // shadow @45%
    inline constexpr uint64_t BADGE_RIM  = 0xfff4f6f8; // identity badge disc

    inline constexpr const char* FONT = "IBM Plex Sans";
    inline constexpr int    RAD_CARD       = 16;
    inline constexpr int    RAD_ROW        = 14;
    inline constexpr double ROUNDING_POWER = 3.0;
    inline constexpr int    MOTION_SPATIAL = 320;
}
```

**Character:** Cool graphite base with vibrant cyan accent. Distinctive shell identity.

### 1.2 Current Palette (v7.0.1 — **WRONG**)

From `common/theme.hpp` at HEAD:

```cpp
namespace NHyprCommon::Theme {
    // Generic Pixel/AOSP palette — washed blue/teal
    inline constexpr uint64_t PANEL               = 0xff132732; // opaque (no glass)
    inline constexpr uint64_t SURFACE             = 0xff172025; // cards
    inline constexpr uint64_t SURFACE_HIGH        = 0xff243944; // raised
    inline constexpr uint64_t STATE               = 0x339acbff; // @20%
    inline constexpr uint64_t ON_SURFACE          = 0xffeef3f5;
    inline constexpr uint64_t ON_SURFACE_STRONG   = 0xfff3f6f7;
    inline constexpr uint64_t ON_SURFACE_VARIANT  = 0xffd1dde1;
    inline constexpr uint64_t ON_SURFACE_DISABLED = 0x61d1dde1; // @38%
    inline constexpr uint64_t PRIMARY             = 0xff9acbff; // washed blue
    inline constexpr uint64_t ON_PRIMARY          = 0xff102333;
    inline constexpr uint64_t ERROR               = 0xffffb4ab; // salmon
    inline constexpr uint64_t ON_ERROR            = 0xff690005;
    inline constexpr uint64_t ERROR_CONTAINER     = 0xff93000a;
    inline constexpr uint64_t OUTLINE             = 0x33e0f0f8; // @20%
    inline constexpr uint64_t SHADOW              = 0x73000000;
    inline constexpr uint64_t BADGE_RIM           = 0xfff5f7f8;

    inline constexpr const char* FONT = "Roboto";
    inline constexpr int    RAD_CARD       = 24; // too large
    inline constexpr int    RAD_ROW        = 12;
    inline constexpr double ROUNDING_POWER = 2.0;
    inline constexpr int    MOTION_SPATIAL = 320;
}
```

**Character:** Generic Material 3 blue-teal. No distinctive identity.

### 1.3 Mapping for Restoration

Map the old glass·ink values to the new semantic role names:

| Semantic Role (keep) | v6.9.2 Value (restore) | Current (wrong) |
|----------------------|------------------------|-----------------|
| `PANEL` | `GLASS` 0x9e0f1218 | 0xff132732 |
| `SURFACE` | `GLASS` 0x9e0f1218 | 0xff172025 |
| `SURFACE_HIGH` | `FILL2` 0x17ffffff | 0xff243944 |
| `STATE` | `ACCENT_DIM` 0x2932d6ff | 0x339acbff |
| `ON_SURFACE` | `INK` 0xffe4e8ee | 0xffeef3f5 |
| `ON_SURFACE_STRONG` | `TITLE` 0xffeef1f5 | 0xfff3f6f7 |
| `ON_SURFACE_VARIANT` | `SUB` 0xff98a2ac | 0xffd1dde1 |
| `PRIMARY` | `ACCENT` 0xff32d6ff ⭐ | 0xff9acbff |
| `ON_PRIMARY` | `ON_ACCENT` 0xff07161c | 0xff102333 |
| `ERROR` | `URGENT` 0xffff8a5c | 0xffffb4ab |
| `ON_ERROR` | (derive) 0xff2b0900 | 0xff690005 |
| `ERROR_CONTAINER` | (derive) 0xcc4d1f00 | 0xff93000a |
| `OUTLINE` | `LINE` 0x17dcebff | 0x33e0f0f8 |
| `ON_SURFACE_DISABLED` | (derive: SUB @38%) 0x6198a2ac | 0x61d1dde1 |

**Action:** Restore glass·ink palette to `common/theme.hpp`, keeping semantic role names but substituting v6.9.2 color values.

---

## 2. Typography

| Version | Font |
|---------|------|
| v6.9.2 | **IBM Plex Sans** ⭐ |
| Current | Roboto |
| AOSP | Roboto |

**Finding:** The change to Roboto aligns with AOSP but loses the shell's distinctive IBM Plex Sans character.

**Action:** Restore `FONT = "IBM Plex Sans"` in `common/theme.hpp`.

---

## 3. Geometry and Radii

### 3.1 Card Radius

| Version | Card Radius | Rounding Power |
|---------|-------------|----------------|
| v6.9.2 | **16px** | **3.0** |
| Current | 24px | 2.0 |
| **AOSP Material 3** | **12dp** | N/A |

**Finding:** Current 24px is **wrong for Pixel/AOSP**. Material 3 cards use **12dp** corner radius. The v6.9.2 value of 16px was closer to the heritage look but not AOSP-compliant.

**Decision point:** User wants "exactly as Pixel/AOSP" BUT "with proper color palette from previous old hyprnotify version."

**Proposed compromise:**
- **Card radius: 16px** (heritage v6.9.2 value, not AOSP's 12dp) — keeps the glass·ink visual identity
- **Rounding power: 3.0** (heritage superellipse, not Material 3's 2.0)
- **Shade radii: match card radius** (not 32/4)

**Rationale:** The user wants AOSP *behavior* (bundling at 4, conversation ranking, snooze) but the *old hyprnotify visual identity* (glass·ink palette, IBM Plex Sans, 16px/3.0 geometry). The "exactly as Pixel/AOSP" applies to the functional model, not the material.

### 3.2 hyprnotify/ui.hpp Constants

Current (wrong):

```cpp
inline constexpr double PIXEL_SHADE_RADIUS = 32;    // shade outer radius
inline constexpr double PIXEL_INTERNAL_RADIUS = 4; // internal card joints
```

**Action:**
```cpp
inline constexpr double PIXEL_SHADE_RADIUS = 16;    // match heritage card radius
inline constexpr double PIXEL_INTERNAL_RADIUS = 16; // consistent joints
```

---

## 4. Bundling Threshold ⭐ CRITICAL

### Current Implementation

`hyprnotify/center.cpp` line 27:

```cpp
inline constexpr size_t AUTOGROUP_AT = 2;
```

From `main.cpp` docstring:

> "Declared groups win over automatic application/section groups, **which form at two**"

### AOSP Specification

Per research and AOSP source: notifications from the same app **bundle at 4**.

**Action:** Change `AUTOGROUP_AT` from `2` to `4` in `center.cpp`.

---

## 5. Timed Mutes (Feature Regression) ⭐

### v6.9.2 Behavior

From `hyprnotify-shade-model.md` memory:

> "6.9.0 ergonomics: the ⋮ manage panel replaced the ⊘ ◷ ★ strip, **timed mutes**, the snooze undo row"

### Current Behavior

From `hyprnotify/policy.cpp`:

```cpp
// s_silenced: app key → epoch-second expiry
// expiry = 0 means persistent
```

The hold surface (manage panel) now **only writes `expiry = 0`** (persistent Silent). The code can still *read* nonzero expiry values from legacy policy.tsv files for backward compatibility, but there's **no UI affordance** to *set* a timed mute.

**Finding:** This is a **feature regression**. The 6.9.0 timed mutes (e.g., "silence for 1 hour") were removed.

**Action Required:**

1. **Add menu entries** in `hyprnotify/ui.hpp` `menuEntries()` for time-bounded silence options (15min, 1h, 2h, until evening, etc.)
2. **Update input handler** in `input.cpp` to write nonzero expiry timestamps to policy state
3. **Visual indication** in row rendering when an app is temporarily silenced (not just persistent)
4. **Timer to refresh** silenced apps when their expiry deadline passes

**AOSP snooze times** (from ROM `config_notification_snooze_times`): Typically 15min, 30min, 1h, 2h. The existing `cfg.snoozeSeconds` config handles *card* snooze (defer until later); this is *app/conversation* silence (mute for duration).

---

## 6. Other Layout Constants (hyprnotify/ui.hpp)

### Current vs. AOSP

| Constant | Current | AOSP Spec | Action |
|----------|---------|-----------|--------|
| `EDGE` | 16 | 16dp ✅ | Keep |
| `PADX`, `PADY` | 16 | 16dp ✅ | Keep |
| `ROW_ICON` | 40 | 24dp (icons) / 104dp (large avatar) | Keep 40 (conversation avatar size) |
| `BTN_H` | 48 | 48dp min touch target ✅ | Keep |
| `BTN_ICON` | 20 | 24dp | Consider 24 (minor) |

**Finding:** Most layout constants are AOSP-compliant. `ROW_ICON = 40` is reasonable for conversation avatars (between standard 24dp icon and 104dp large avatar).

---

## 7. Animation Timings

Current from `common/theme.hpp`:

```cpp
inline constexpr int MOTION_SPATIAL = 320; // panel open/close, card arrival
```

AOSP spec:
- Small: 150–200ms
- Standard: 225ms
- **Large/spatial: 300–400ms** ✅

**Finding:** 320ms is within AOSP range. No change needed.

---

## 8. Version Bump

Current: `hyprnotify/v7.0.1`  
After fixes: `hyprnotify/v7.1.0` (MINOR bump for restored features + palette)

Update:
- `hyprpm.toml` version field
- `hyprnotify.hpp` `VERSION` constant
- Tag after verifying gate passes

---

## Summary of Required Changes

| File | Change | Type |
|------|--------|------|
| `common/theme.hpp` | Restore glass·ink palette values | Color |
| `common/theme.hpp` | `FONT = "IBM Plex Sans"` | Typography |
| `common/theme.hpp` | `RAD_CARD = 16`, `RAD_ROW = 14` | Geometry |
| `common/theme.hpp` | `ROUNDING_POWER = 3.0` | Geometry |
| `hyprnotify/ui.hpp` | `PIXEL_SHADE_RADIUS = 16` | Layout |
| `hyprnotify/ui.hpp` | `PIXEL_INTERNAL_RADIUS = 16` | Layout |
| `hyprnotify/center.cpp` | `AUTOGROUP_AT = 4` | **Behavior** ⭐ |
| `hyprnotify/ui.hpp` | Add timed-mute menu entries | **Feature** ⭐ |
| `hyprnotify/input.cpp` | Handle timed-mute selection | **Feature** ⭐ |
| `hyprnotify/policy.cpp` | (no change, already supports nonzero expiry) | — |
| `hyprnotify/row.cpp` | Visual indication for timed silence | **Feature** ⭐ |
| `hyprnotify/model.cpp` | Refresh expired silences on tick | **Feature** ⭐ |
| `hyprnotify.hpp` | `VERSION = "7.1.0"` | Metadata |
| `hyprpm.toml` | `version = "7.1.0"` | Metadata |

---

## Implementation Order

1. **Theme restoration** (`common/theme.hpp`) — palette + font + radii
2. **Layout constants** (`hyprnotify/ui.hpp`) — shade/internal radii
3. **Bundling threshold** (`center.cpp`) — AUTOGROUP_AT = 4
4. **Timed mutes** (multi-file) — menu + input + visual + refresh logic
5. **Version bump** + gate + tag

Each commit verifiable with `devtools/stress.sh` gate.

---

## Appendix: AOSP Reference

- **Card corner radius:** 12dp (Material 3)
- **Standard padding:** 16dp
- **Min touch target:** 48dp
- **Icon size:** 24dp
- **Large avatar:** 104dp recommended
- **Bundling threshold:** 4 notifications from same app
- **Animation timings:** 150–200ms small, 225ms standard, 300–400ms large
- **Snooze options:** 15min, 30min, 1h, 2h (typical)
- **Ranking:** Critical > priority conversations > conversations > normal > silent

---

## 9. Visual Analysis from Pixel Screenshots

### 9.1 notification_shade.jpg — The Notification Shade

**Observed elements:**

1. **Conversation grouping (Telegram):**
   - Multiple Telegram notifications are **bundled together** under one app header
   - Each message shows: **circular avatar (left)**, sender name, message preview, timestamp
   - Messages are stacked vertically with subtle dividers
   - The bundle shows a **count indicator** and expand/collapse chevron
   - **Finding:** Confirms AOSP bundles multiple notifications from same app

2. **Individual notifications:**
   - Gmail notification shows: **app icon (left)**, sender, subject line, preview text
   - **Two action buttons** at bottom: "Archive" and "Reply" 
   - Buttons have **rounded pill shape**, text labels, no visible icons
   - **Finding:** Action buttons are text-only pills, not icon+text combos

3. **Card styling:**
   - Notifications have **subtle rounded corners** (appears to be ~12-16px radius)
   - Cards have **minimal elevation** (1-2dp shadow)
   - Background is **dark gray/blue** (not pure black)
   - Text hierarchy: **title bold/white**, body lighter gray, timestamps very subtle
   - **No visible hairline borders** between cards

4. **Spacing:**
   - Cards have **consistent padding** around content (~16dp)
   - **Gap between cards** appears ~8-12px
   - Avatar to text gap: ~12-16px
   - Action buttons have internal padding and gap between them

5. **Typography:**
   - Headers/senders: **Medium weight, ~14-15sp**
   - Body text: **Regular weight, ~13-14sp**
   - Timestamps: **Light/regular, ~11-12sp, muted color**

### 9.2 notification_popup.jpg — Heads-Up Banner

**Observed elements:**

1. **Single notification popup:**
   - Appears at **top of screen** (not top-right as in current hyprnotify)
   - **Full width** of screen with horizontal margins
   - Shows app icon, sender, message preview
   - **Compact height** — single line of text visible

2. **Styling:**
   - **Same card radius** as shade notifications
   - **Higher elevation** than shade cards (8dp shadow visible)
   - Background matches shade card color
   - **Dismiss gesture** indicator may be present (hard to see)

3. **Position:**
   - **Top-center of screen**, not top-right corner
   - **Finding:** Current hyprnotify places popups top-right; Pixel uses top-center full-width

### 9.3 notification_hold_menu.jpg — Long-Press Menu

**Observed elements:**

1. **Menu structure:**
   - Appears as **modal overlay** over the notification
   - Shows app icon and name at top
   - **Three options visible:**
     - "Prioritize" or "Default" (conversation priority)
     - "Turn off notifications" (silence)
     - "All [App] notifications" (settings link)

2. **Styling:**
   - Menu has **rounded corners** (same radius as cards)
   - **Darker background** than notification cards (elevation/scrim)
   - Options are **list items** with text labels
   - Each option has **~48-52dp height** (touch target)
   - **Icons on the left** of each option (bell, settings, etc.)
   - **Radio button or checkmark** on right for selected state

3. **Timed silence UI:**
   - **NOT visible in this screenshot** — may require submenu or different flow
   - AOSP's "Turn off notifications" typically opens a secondary menu with time options
   - **Finding:** Timed mutes may be in a submenu, not top-level menu

### 9.4 Additional Visual Findings

**Color palette observations:**
- Shade background: **Dark blue-gray ~#1a1f28** (not pure black, matches current PANEL)
- Card surface: **Slightly lighter ~#252a34** (subtle contrast from background)
- Primary text: **Near-white ~#e8ecf0**
- Secondary text: **Medium gray ~#9ca4ae**
- Accent (links, buttons): **Light blue ~#7bb8ff to #9ac9ff**
- Dividers: **Very subtle, ~10-15% white overlay**

**Icon sizing:**
- App icons in shade: **~32-40px** (medium)
- Avatars in conversations: **~40-48px** (larger)
- Action button icons: **~20-24px** (if present)
- Menu option icons: **~24px**

**Corner radius consistency:**
- All cards appear to use **same radius** (~12-16px)
- Action buttons: **Full pill** (radius = height/2)
- Menu: **Matches card radius**

**Conversation message grouping:**
- Multiple messages from **same sender** stack without repeating avatar
- **Avatar shown once** for most recent message from that sender
- Previous messages from same sender: **indented slightly**, no avatar
- **Different senders**: Each gets their own avatar on the left

**Action button layout:**
- Buttons aligned **horizontally** at card bottom
- **Equal width** or auto-sized to content
- **Gap between buttons**: ~8-12px
- Button height: **36-40px**
- Button corner radius: **Full pill** (18-20px for 36-40px height)

### 9.5 Discrepancies vs. Current hyprnotify Implementation

| Element | Pixel Screenshot | Current hyprnotify | Action Needed |
|---------|------------------|-------------------|---------------|
| **Popup position** | Top-center, full-width | Top-right, fixed width | Consider top-center (desktop adaptation) |
| **Card borders** | None visible | May have hairlines | Verify no borders or very subtle |
| **Action buttons** | Text-only pills | Icon + text? | Audit button implementation |
| **Conversation avatars** | Deduplicated per sender | Every message? | Verify deduplication logic |
| **Menu icons** | Icons on left | Icons on left ✓ | Keep |
| **Timed silence** | Submenu (not shown) | Removed | **RESTORE with submenu** |
| **Card radius** | ~12-16px uniform | 24px (wrong) | Change to 16px (heritage) |
| **Bundling** | Multiple apps bundled | Bundles at 2 (wrong) | Change to 4 |

### 9.6 Updated Recommendations

Based on visual analysis:

1. **Card radius: 16px is correct** — screenshots show ~12-16px range; 16px matches heritage and looks appropriate
2. **No visible borders** — remove or minimize hairlines between cards
3. **Action buttons**: Verify they're **text-only pills**, not icon+text (unless media controls)
4. **Timed silence**: Implement as **submenu** under "Turn off notifications", not top-level options
5. **Conversation deduplication**: Verify avatars don't repeat for consecutive messages from same sender
6. **Color palette**: Current Pixel colors (~#172025 SURFACE) are actually accurate to screenshots, BUT user wants **glass·ink heritage** instead
7. **Popup position**: Desktop keeps top-right (Wayland convention), not mobile's top-center full-width

### 9.7 Timed Silence UI Specification

Based on AOSP pattern (not visible in screenshots but standard):

```
Long-press notification → Manage panel opens
├─ Priority / Default / Silent (radio selection)
├─ Turn off notifications → Opens submenu:
│  ├─ For 15 minutes
│  ├─ For 1 hour  
│  ├─ For 2 hours
│  └─ Until I turn back on (persistent)
└─ All [App] notifications (settings)
```

**Implementation plan for timed mutes:**
1. Add "Silence for..." submenu in manage panel
2. Time options: 15min, 1h, 2h, persistent (map to AOSP snooze times)
3. Write nonzero expiry to policy.cpp (already supported for reading)
4. Visual indicator: muted icon in row when app is silenced with time remaining
5. Refresh timer: check expired silences on each model tick

---

**Next Step:** Present this complete audit (with visual findings) to the user for approval before touching any files.
