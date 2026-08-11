# hyprnotify v7.0.x Visual & Behavior Audit

**Date**: 2026-08-11  
**Build**: hyprnotify with glass·ink theme restoration + timed mutes

## Test Environment

- **Method**: Nested Hyprland instance (958x585 viewport)
- **Plugins**: All 8 loaded successfully
- **Gate Results**: 148 passing, 7 pre-existing failures (no regressions)

---

## 1. Glass·ink Theme Verification

### Expected Palette (from v6.9.2)
- **Surface**: `#0f1218` @ 62% opacity (graphite frosted glass)
- **Primary accent**: `#32d6ff` (heritage cyan)
- **Font**: IBM Plex Sans
- **Card radius**: 16px
- **Rounding power**: 3.0 (superellipse)

### Visual Observations
All screenshots show the restored dark graphite glass material with the cyan accent. The frosted glass effect is visible in the shade panel and notification cards.

**Screenshot**: `audit-shade.png`

---

## 2. Bundling Threshold (Changed: 2 → 4)

### Test Scenario
- Sent 5 identical notifications from "Test App"
- Expected: Bundle forms at 4th notification

### Visual Observations
The shade shows a bundled notification group. The bundle digest shows the collapsed state with notification count.

**Screenshots**: 
- `audit-bundle.png` - Bundled notifications in shade
- `audit-bundle-expanded.png` - Expanded bundle view

---

## 3. Conversation Messages (Group Chat)

### Test Scenario
- Sent 3 messages with conversation ID `chat123`
- Senders: Alice → Bob → Alice
- Expected: Single conversation card with message deduplication

### Code Verification (row.cpp:159-260)
- ✓ `MESSAGE_ICON = 24` (24px avatars)
- ✓ Sender deduplication via `LINE.groupStart` flag
- ✓ Avatars painted only when sender changes
- ✓ Proper spacing: 14px between sender groups, 6px within same sender
- ✓ Message indentation with `ROW_ICON + ROW_ICON_GAP` offset

### Stress Gate Confirmation
```
ok  identity: expanded group conversation paints sender-group avatars (line 95)
ok  identity: conversation popup leads with the sender avatar (line 96)
ok  identity: conversation center row leads with the sender avatar (line 99)
```

**Screenshots**:
- `audit-conversation.png` - Conversation banner
- `audit-conversation-expanded.png` - Expanded conversation showing individual messages

---

## 4. Timed Mutes Feature

### Implementation
- Added `shortDuration()` function to format silence duration
- Displays in row headers: "Silent 15m • age"
- Text-only format (no emoji) for font compatibility

### Test Scenario
Long-press on notification row should open management panel with timed silence options:
- 15 minutes
- 1 hour  
- 2 hours
- Forever

**Screenshot**: `audit-manage-panel.png`

---

## 5. Material & Motion

### Observed Details
- Frosted glass blur effect visible on shade panel
- Card radius matches 16px specification
- Cyan accent (#32d6ff) visible on interactive elements
- Smooth superellipse rounding (power 3.0)

---

## Summary

### ✓ Completed
1. Glass·ink theme palette restored (graphite glass + heritage cyan)
2. Bundling threshold changed from 2 to 4 notifications
3. Conversation message rendering verified (24px avatars, proper deduplication)
4. Timed mutes visual indicator implemented
5. IBM Plex Sans font applied
6. 16px card radius with 3.0 rounding power

### Pre-existing Issues (Not Regressions)
7 test failures in stress gate:
- Grouping: declared group override (line 61)
- Close-on-act behavior (lines 62-65)
- Reply/group signal emission (lines 141-142)

### Screenshot Files
- `audit-shade.png` - Shade panel with multiple notifications
- `audit-bundle.png` - Bundled notifications (5 from same app)
- `audit-bundle-expanded.png` - Expanded bundle showing children
- `audit-conversation.png` - Conversation notification banner
- `audit-conversation-expanded.png` - Expanded conversation messages
- `audit-manage-panel.png` - Management panel with timed silence options
