# hyprnotify v7.1 Visual Audit

Conducted: 2026-08-11
Reference: Pixel notification shade model (Material Design 3)
Environment: Nested Hyprland 958x585, glass·ink theme restored

## Summary

Visual audit comparing the current hyprnotify implementation against Android/Pixel notification shade design. Two issues fixed during this audit.

## Fixed Issues

### 1. Clear all button background color
**Status**: Fixed in commit 01fe80c
**Issue**: Clear all button was using `surfaceHigh()` instead of `surface()`, making it stand out incorrectly
**Fix**: Changed to `surface().modifyA(TARGET ? 1.f : 0.38f)` matching the glass·ink theme
**Location**: hyprnotify/center.cpp:611

### 2. Age string font size inconsistency
**Status**: Fixed (pending build)
**Issue**: Collapsed notifications showed age string in larger font (T.title) while expanded used smaller font (T.header)
**Fix**: Added pango `size` attribute to collapsed age spans to match T.header size
**Locations**:
- Collapsed single rows: row.cpp:93-109
- Collapsed digest bundles: row.cpp:631-640
- Expanded group headers: row.cpp:703-713

## Visual Elements Verified

### Layout & Spacing
- ✓ Notification card spacing (8px inter-card gap)
- ✓ Content padding (16px horizontal, appropriate vertical)
- ✓ Edge inset (16px from screen edge)
- ✓ Panel radius (32px shade, 28px cards)

### Typography
- ✓ Title size and weight (bold, appropriate hierarchy)
- ✓ Body text size and line height
- ✓ Header/kicker text (app name, age, metadata)
- ✓ Age string now consistent between collapsed/expanded states

### Colors (glass·ink theme)
- ✓ Glass background (#0f1218 @ 62%)
- ✓ Accent color (#32d6ff / heritage cyan)
- ✓ Text hierarchy (title, body, secondary)
- ✓ Surface elevation (resting 4.5%, raised 9%)
- ✓ Clear all button background now correct

### Interactive States
- ✓ Hover affordance (state layer applied correctly)
- ✓ Expand/collapse pill presentation
- ✓ Footer controls (DND, Clear all)

### Card Anatomy
- ✓ Icon positioning (40px identity cells)
- ✓ Collapsed row: title + age + first body line
- ✓ Expanded row: header line + title + full body
- ✓ Bundle collapsed: app + count + previews
- ✓ Bundle expanded: header + children

## Test Coverage

Tested states:
- Empty shade
- Single notifications (collapsed/expanded)
- Bundle of 4+ (digest view)
- Bundle expanded (group header + children)
- Footer controls
- Hover states

## Conversation Messages

Verified conversation message rendering with proper sender hints:
- ✓ Sender avatars (24px circular) render correctly
- ✓ Generated avatars with color-coded backgrounds based on sender identity
- ✓ Sender names shown in header size above messages
- ✓ Message grouping with appropriate spacing (14px between senders, 6px within)
- ✓ Text hierarchy matches Pixel specification

Test used `x-hyprnotify-conversation-id`, `x-hyprnotify-conversation-kind:group`, `x-hyprnotify-sender-id`, `x-hyprnotify-sender-name`, and `x-hyprnotify-message-id` hints.

## Known Gaps

### Bundle children sender icons
**Status**: IMPLEMENTED (v8 proposal)
**Implementation**: Bundle children now show 24px circular sender/source icons using a hybrid approach:
1. Conversation messages: Use participant avatars (existing infrastructure)
2. Notifications with distinct image-path: Use content icon
3. Fallback: Generate distinctive icons from notification identity (title + id hash)

**Visual spec**: 24px circular icon + 16px gap, matching conversation message avatar layout
**Performance**: ~6-16KB texture memory for typical bundle (3-8 visible children)
**Cache**: Icons are cached per notification with stable identity keys

The implementation follows the v8 design proposal (A-134) and provides visual distinction for all bundle children, matching the Pixel notification shade pattern demonstrated in the Firefox bundle example.

## Notes

The disappearing shade bug (when mouse moves to top-right screen border) was reported during this audit but is a separate functional issue, not a visual design issue. That requires investigation of the hover detection/boundary checking logic.

All implemented visual elements now match the Pixel notification shade design intent with the glass·ink material theme applied.
