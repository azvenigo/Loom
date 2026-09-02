#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

//////////////////////////////////////////////////////////////////////////////////////////////////
// Dashboard - the operator UI, embedded in the binary and served at "/".
//
// EMBEDDED rather than read from disk: Loom is meant to be one file you drop on the Linux box and
// run under systemd. An asset directory is one more thing to get wrong in a unit file, and a
// dashboard that 404s because the working directory moved is worse than no dashboard.
//
// SERVED BY LOOM rather than opened from disk: same origin means the page calls the API with no
// CORS preflight and no configuration. It also means this cannot be a hosted page - a sandboxed
// remote page cannot reach 127.0.0.1.
//
// Vanilla everything. No framework, no build step, no CDN - consistent with a project that vendors
// its dependencies, and it keeps this file editable in place.
//
//------------------------------------------------------------------------------------------------
// DESIGN NOTES, because the reasoning is not recoverable from the CSS:
//
//   RANKING IS THE POINT, SO IT IS VISIBLE. Every result carries a left accent rail whose opacity
//   scales with its score relative to the best hit, and matched terms are marked in the text. The
//   question this tool exists to answer is "is Loom's ranking good enough to replace the
//   hand-maintained cluster index files", and that is unanswerable from a list that hides why
//   things ranked where they did.
//
//   EVERYTHING IS A JOT. A name (slug) just makes one addressable by [[link]] and mergeable by
//   upsert - it is not a separate tier of record, and the UI does not call it one out.
//
//   TIME IS SHOWN LOCALLY. Ids are UTC microseconds, but the imported entries were written in local
//   wall-clock time. Formatting happens in the browser from the id, so an entry written at 13:09
//   reads 13:09 rather than the 20:09 a raw UTC field shows. Recent groups by day, which is what
//   makes two years of imported history legible instead of one flat scroll.
//
//   CHROME RECEDES. Metadata is small, monospaced and faint; content is the only thing carrying
//   full contrast.
//////////////////////////////////////////////////////////////////////////////////////////////////

inline const char* LoomDashboardHtml()
{
    return
R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Loom</title>
<link rel="icon" type="image/png" href="/icon.png">
<style>
:root{
  --bg:#faf9f6; --panel:#fff; --sunk:#f3f1ec;
  --ink:#1c1a17; --body:#3a3630; --dim:#6f6a61; --faint:#a49d92;
  --line:#e7e3da; --line-soft:#f0ece4;
  --accent:#6d5bd0; --accent-ink:#4e3fae; --accent-wash:#f0edfd;
  --warn:#a1620c; --warn-wash:#fdf6ec; --warn-line:#e8c99a;
  --bad:#a32c22; --bad-wash:#fdf1f0; --bad-line:#e7b3ad;
  --good:#2c6b45; --good-wash:#f0f8f3; --good-line:#a9d4bd;
  --mark:#fdf0a8;
  /* SECOND GRADIENT STOP for each filled color. Every palette declares these three and the button
     rules build the ramp from them - see the "filled controls" block. They are plain hex on
     purpose rather than a color-mix() off the base: a var() that fails to substitute inside
     `background` does NOT fall back to the previous declaration, it unsets the property, and an
     unstyled Save button on an old browser is a worse trade than three extra lines per palette.
     RULE FOR PICKING ONE: filled controls print --panel on top of this ramp, so BOTH stops have to
     stay far from --panel in luminance. On a dark palette that means the second stop can be much
     brighter than the base and the contrast only improves. On a LIGHT one it must not be - a
     lightened stop is exactly where white-on-color falls apart, so light palettes shift HUE at
     roughly equal lightness instead (violet into magenta, ochre into rust) and let the inset
     highlight, not the ramp, do the lit-from-above work. Measured: every stop here clears 3.2:1
     against its own --panel. */
  --accent2:#7b4fd6; --good2:#2f7d5c; --warn2:#b0561a;
  /* Second stop for the one destructive filled control. Defaults to --bad, i.e. a flat
     fill, so a palette that has not thought about it looks exactly as it did. */
  --bad2:var(--bad);
  /* The blue that means "normal" - a priority level, not a state. Every other semantic colour
     here already existed; there was no neutral-informational hue to code Normal with. */
  --info:#3b6fd4; --info-wash:#e8eefb; --info-line:#b9cdf0;
  /* The editor's text boxes. On a light palette this is just white; on a dark one it's a LIGHT
     TINT OF THE PALETTE'S OWN HUE rather than pure white, which at ~72% of white's luminance
     stops the field being a floodlight in a dark room while keeping ink contrast above 10:1. */
  --field-bg:#ffffff; --field-ink:#1c1a17; --field-dim:#6f6a61;
  /* TODO panel ground - see .ov-todo. Two identical stops means a flat fill, which is what the
     eight quiet palettes have always had. */
  --todo-top:var(--warn-wash); --todo-bot:var(--warn-wash);
  --todo-edge:var(--warn-line); --todo-bar:var(--warn); --todo-glow:var(--warn);
  /* HIGHLIGHT CARD ground - see .ov-card.hi. Same trick as --todo-*: two identical stops is a
     flat fill, so every palette that doesn't override these renders exactly the plain
     background:var(--accent) the card had before. */
  --hi-a:var(--accent); --hi-b:var(--accent); --hi-ang:110deg;
  /* THE ONE CALL TO ACTION - see button.warnfill. Split out of --warn2/--warn because the button
     and the semantic warning color don't have to be the same thing. --warn still has to work as
     the TODO panel's bar, its badge and Normal-priority ink, and a two-hue ramp picked to make a
     button pop reads as mud in all three. Defaults reproduce the old --warn2 -> --warn ramp. */
  --cta-a:var(--warn2); --cta-b:var(--warn); --cta-edge:var(--warn); --cta-ang:170deg;
  /* Tag pills, defaulting to the sunk/dim pair they used to name directly. Separate because a
     pill wants MORE saturation than --dim (which is running body text) once the ground is a tint
     rather than a neutral. */
  --tag-bg:var(--sunk); --tag-ink:var(--dim);
  /* Section kickers - DISTRIBUTION, ACTIVITY, TAGS IN USE. See the .eyebrow rule. */
  --eyebrow-ink:var(--faint);
  --mono:ui-monospace,SFMono-Regular,"SF Mono",Menlo,Consolas,monospace;
  --sans:system-ui,-apple-system,"Segoe UI",Roboto,"Helvetica Neue",sans-serif;
  --r:7px;
}
/* Paper/Midnight are the original pair (purple accent, light/dark) - the old "Loom" auto-dark and
   "Midnight" sat almost on top of each other once the OS was in dark mode, which was the
   complaint that started this; Midnight now covers that end on its own, :root above is "Paper".
   The rest are three more light/dark pairs, adapted from the terminal color tables in
   ZLibraries/Common/zhelpers/FormatHelpers.cpp (Style::GetSchemes - "nord"/"earth"/"twilight"
   entries) rather than invented from nothing: real, already-liked schemes, re-tuned for web
   contrast (those tables are console ANSI colors on a near-black background only - the *-light
   variants here are new, built to the same palette identity, not copied from anywhere). Alex
   wants to try a few and narrow down, so this errs toward offering options rather than picking. */
:root[data-palette="nord"]{
  --bg:#eceff4; --panel:#ffffff; --sunk:#e5e9f0;
  --ink:#2e3440; --body:#3b4252; --dim:#5e6779; --faint:#8b93a3;
  --line:#d3d9e3; --line-soft:#e2e6ee;
  --accent:#5e81ac; --accent-ink:#456384; --accent-wash:#e1e9f2;
  --warn:#a6791e; --warn-wash:#f7edd6; --warn-line:#e3cd94;
  --bad:#bf616a; --bad-wash:#fae4e5; --bad-line:#e6b3b8;
  --good:#4c7a3d; --good-wash:#e6f0e0; --good-line:#b5d1a5;
  --mark:#f2dfa0;
  --accent2:#6b73b8; --good2:#3f7a56; --warn2:#a8651e;
  --info:#4a7ba8; --info-wash:#e1e9f2; --info-line:#b7c9dd;
  --field-bg:#ffffff; --field-ink:#2e3440; --field-dim:#5e6779;
}
:root[data-palette="nord-dark"]{
  --bg:#2e3440; --panel:#3b4252; --sunk:#333a48;
  --ink:#eceff4; --body:#d8dee9; --dim:#9aa5b8; --faint:#616e88;
  --line:#4c566a; --line-soft:#434c5e;
  --accent:#88c0d0; --accent-ink:#a9d4e0; --accent-wash:#2a3a41;
  --warn:#ebcb8b; --warn-wash:#3d3624; --warn-line:#5c5334;
  --bad:#e0949b; --bad-wash:#3a2429; --bad-line:#5c3840;
  --good:#a3be8c; --good-wash:#2c3826; --good-line:#455339;
  --mark:#4a4020;
  --accent2:#a8d6e2; --good2:#c0d4ab; --warn2:#f2dcaa;
  --info:#81a1c1; --info-wash:#26303d; --info-line:#3f5062;
  --field-bg:#d8e0ec; --field-ink:#232a36; --field-dim:#5b6577;
}
:root[data-palette="earth"]{
  --bg:#f3e8d5; --panel:#faf3e6; --sunk:#ecdfc4;
  --ink:#2c2015; --body:#4a3820; --dim:#7c6b4e; --faint:#a8987a;
  --line:#dcc9a0; --line-soft:#e8d8b6;
  --accent:#a3672f; --accent-ink:#7d4d20; --accent-wash:#ecdcb8;
  --warn:#a1620c; --warn-wash:#f5e6c8; --warn-line:#d8b981;
  --bad:#a32c22; --bad-wash:#f5d9d2; --bad-line:#d99a8e;
  --good:#5c7a3d; --good-wash:#e2ecd8; --good-line:#a9c78f;
  --mark:#f0d98a;
  --accent2:#ad4a2a; --good2:#4f7d52; --warn2:#a8541a;
  --info:#3d6f9e; --info-wash:#e2ecf4; --info-line:#a8c4da;
  --field-bg:#fffcf3; --field-ink:#2c2015; --field-dim:#7c6b4e;
}
:root[data-palette="earth-dark"]{
  --bg:#1a140d; --panel:#241c13; --sunk:#181209;
  --ink:#f0e6d2; --body:#d2bea0; --dim:#a8987a; --faint:#786a52;
  --line:#4a3820; --line-soft:#3a2c1a;
  --accent:#dcb478; --accent-ink:#eecb92; --accent-wash:#3a2e18;
  --warn:#c87832; --warn-wash:#3a2612; --warn-line:#5c4020;
  --bad:#e07a6a; --bad-wash:#3a1c1a; --bad-line:#5c302e;
  --good:#88aa55; --good-wash:#26301a; --good-line:#3e4c2c;
  --mark:#4a3818;
  --accent2:#efd3a2; --good2:#a8c876; --warn2:#e09a58;
  --info:#7fa8c8; --info-wash:#16252e; --info-line:#2e4553;
  --field-bg:#ebe0cd; --field-ink:#241c13; --field-dim:#7a6a4e;
}
/* Twilight is no longer "the purple one" - it's the specific soft-indigo look Alex picked out of
   a reference screenshot, and it's the only palette that spends all five of the new token groups
   at once. Four things carry that look, and none of them are the base --accent:
     1. a near-white lavender ground (--bg) under white panels, with borders so light they read as
        separations rather than as lines;
     2. ONE two-hue call to action - burnt orange into plum, left to right (--cta-*). It is the
        only left-to-right ramp in the file, which is exactly why it reads as the button;
     3. the highlight card as indigo into teal (--hi-*) rather than a flat fill, so the one card
        that isn't white isn't just a colored rectangle either;
     4. saturated ink on the quiet things - kickers at --eyebrow-ink and tag pills at --tag-ink,
        both real indigo instead of a gray. Small type is where a pastel palette usually goes
        limp; these are the two places it doesn't get to.
   Every color that prints --panel on top was checked against it: the CTA's orange stop is the
   tight one at 4.5:1, which is why it's a burnt orange and not the reference's brighter one -
   at #f97316 white on it lands near 2.5:1 and the label stops being readable over the left third
   of the button. The teal end of the highlight card is 3.4:1 and carries no body text by
   layout - the number and the caption both sit over the indigo half. */
:root[data-palette="twilight"]{
  --bg:#f6f5fe; --panel:#ffffff; --sunk:#efedfd;
  --ink:#1c1839; --body:#3d3866; --dim:#6a6394; --faint:#867eab;
  --line:#e7e4f8; --line-soft:#f1effc;
  --accent:#5b4ce6; --accent-ink:#4438cc; --accent-wash:#edecfe;
  --warn:#b3730c; --warn-wash:#fdf4e4; --warn-line:#f0d7a8;
  --bad:#d4344f; --bad-wash:#fdecef; --bad-line:#f5bcc6;
  --good:#0f8f74; --good-wash:#e7f8f3; --good-line:#a9e2d3;
  --mark:#ffe9a3;
  --accent2:#7d4ae8; --good2:#1b8f56; --warn2:#c98a1a; --bad2:#e04a86;
  --info:#3f6fe0; --info-wash:#eaf0fe; --info-line:#c2cef6;
  --field-bg:#ffffff; --field-ink:#1c1839; --field-dim:#6a6394;
  --hi-a:#5b53ea; --hi-b:#0f9d8e; --hi-ang:105deg;
  --cta-a:#cf4a15; --cta-b:#6a2c74; --cta-edge:#6a2c74; --cta-ang:100deg;
  --tag-bg:#eeecfd; --tag-ink:#5348ba;
  --eyebrow-ink:#5b4ce6;
  /* Warm, but a wash rather than the slab it was: cream at the top of the panel fading to the
     same white as every other card by the bottom. The old flat --warn-wash over a panel this
     tall was the one thing on the page reading as tan. */
  --todo-top:#fdf5e9; --todo-bot:#ffffff;
  --todo-edge:#f3e6d2; --todo-bar:#e89a2c; --todo-glow:#e8a54a;
}
:root[data-palette="twilight-dark"]{
  --bg:#13111f; --panel:#1c1930; --sunk:#171429;
  --ink:#ede8f5; --body:#c4beda; --dim:#8f88ac; --faint:#615a80;
  --line:#3e2f63; --line-soft:#332852;
  --accent:#b39ddb; --accent-ink:#cbb8e8; --accent-wash:#332852;
  --warn:#e8b968; --warn-wash:#3d3018; --warn-line:#5c4a28;
  --bad:#e0697a; --bad-wash:#3a2028; --bad-line:#5c3440;
  --good:#8fd9a8; --good-wash:#1e3428; --good-line:#325240;
  --mark:#4a3c1c;
  --accent2:#d2bdf0; --good2:#b0ecc4; --warn2:#f5d492;
  --info:#8fa8e8; --info-wash:#1e2440; --info-line:#34406b;
  --field-bg:#e0d9f0; --field-ink:#1b1730; --field-dim:#6d6490;
}
:root[data-palette="midnight"]{
  --bg:#141317; --panel:#1b1a20; --sunk:#232128;
  --ink:#eceae5; --body:#cfcbc3; --dim:#948e85; --faint:#6b665e;
  --line:#2c2a32; --line-soft:#25232a;
  --accent:#a493f5; --accent-ink:#c0b3ff; --accent-wash:#272243;
  --warn:#e0a458; --warn-wash:#2a2114; --warn-line:#5c4520;
  --bad:#e08279; --bad-wash:#2b1917; --bad-line:#5e2f2a;
  --good:#6cc294; --good-wash:#152720; --good-line:#2c5340;
  --mark:#5c5220;
  --accent2:#c3b3ff; --good2:#8fd9ae; --warn2:#f0c288;
  --info:#7fa2f0; --info-wash:#182339; --info-line:#2c3c5e;
  --field-bg:#e2dfe9; --field-ink:#17161c; --field-dim:#6b6878;
}
/* ---- the vivid set ----------------------------------------------------------------------------
   The eight above are quiet schemes where the second gradient stop is just a lighter shade of the
   first, so their buttons gain depth without changing hue. These four spend that second stop on a
   DIFFERENT HUE instead - violet into magenta, indigo into pink, cyan into mint, magenta into
   coral - which is what turns a filled button from "a colored rectangle" into something with a
   ramp across it. Three are dark because that's where a two-hue ramp has room to read; Sorbet is
   the light one that still wants to shout.
   They also redefine the --todo-* ground (see .ov-todo): the default "tint the panel with the
   warning color" rule paints a large olive slab on a saturated dark palette, so these carry their
   own ground instead - a wash of the palette's OWN accent at the top fading into --sunk, which
   puts the panel in the same color family as the cards sitting on it.
   Everything else about them is a normal palette. If one of these is the keeper, the ramp is three
   tokens - it can be lifted into any of the others by editing --accent2/--good2/--warn2 alone. */
:root[data-palette="nebula"]{
  --bg:#0f0b1a; --panel:#191330; --sunk:#140f26;
  --ink:#f0ecfa; --body:#c9c0e4; --dim:#948ab8; --faint:#6b6090;
  --line:#33265c; --line-soft:#2a1f4d;
  --accent:#8b5cf6; --accent-ink:#c4a8ff; --accent-wash:#2a1f4d;
  --warn:#f0b429; --warn-wash:#3a2c10; --warn-line:#5c4620;
  --bad:#f2557a; --bad-wash:#38131f; --bad-line:#5c2438;
  --good:#2fbf8f; --good-wash:#10302a; --good-line:#235c48;
  --mark:#4a2f6b;
  --accent2:#e879c7; --good2:#5fe0b0; --warn2:#f7d774;
  --todo-top:#241a45; --todo-bot:#140f26;
  --todo-edge:#33265c; --todo-bar:#8b5cf6; --todo-glow:#8b5cf6;
  --info:#6c8cff; --info-wash:#1a2145; --info-line:#2e3a6b;
  --field-bg:#ded6f0; --field-ink:#171230; --field-dim:#6b5f8c;
}
/* Aurora replaces an earlier warm "Ember" - a brown-grounded dark theme, which turns out to be the
   one thing a large tinted surface cannot survive: every wash on it reads as mud rather than as a
   color. Nebula and Synthwave are both purple-family, so the third vivid slot goes somewhere cold
   instead: deep slate-teal ground, cyan into mint. */
:root[data-palette="aurora"]{
  --bg:#071619; --panel:#0f2630; --sunk:#0b1e26;
  --ink:#e8f6f7; --body:#b6d4d9; --dim:#7fa3ab; --faint:#557880;
  --line:#1b3f4a; --line-soft:#16333c;
  --accent:#0e9fb8; --accent-ink:#5fd6e8; --accent-wash:#0f3340;
  --warn:#e0a02a; --warn-wash:#33280e; --warn-line:#574618;
  --bad:#f2556a; --bad-wash:#351520; --bad-line:#5c2636;
  --good:#3fbf6f; --good-wash:#0c2e22; --good-line:#1c5740;
  --mark:#16404a;
  --accent2:#4fd9c4; --good2:#74e39a; --warn2:#f2c661;
  --todo-top:#123c48; --todo-bot:#0b1e26;
  --todo-edge:#1b3f4a; --todo-bar:#0e9fb8; --todo-glow:#0e9fb8;
  --info:#3fa9d9; --info-wash:#0e2c3d; --info-line:#1c4358;
  --field-bg:#d3e6ea; --field-ink:#08222b; --field-dim:#4a6a72;
}
:root[data-palette="synth"]{
  --bg:#0d0f1f; --panel:#161a33; --sunk:#111428;
  --ink:#eef1ff; --body:#c3c9ec; --dim:#8f97c4; --faint:#626a9c;
  --line:#2c3363; --line-soft:#232951;
  --accent:#5b63e0; --accent-ink:#9aa4ff; --accent-wash:#1e2450;
  --warn:#d99310; --warn-wash:#33280d; --warn-line:#57451a;
  --bad:#e0344f; --bad-wash:#35131f; --bad-line:#5c2338;
  --good:#1fae7c; --good-wash:#0f2e26; --good-line:#1f5745;
  --mark:#3a2a5c;
  --accent2:#f062c8; --good2:#4de0c0; --warn2:#f5cf5c;
  --todo-top:#1b2149; --todo-bot:#111428;
  --todo-edge:#2c3363; --todo-bar:#5b63e0; --todo-glow:#5b63e0;
  --info:#4fb6f0; --info-wash:#12253d; --info-line:#23415c;
  --field-bg:#d8def2; --field-ink:#121628; --field-dim:#5a6280;
}
:root[data-palette="sorbet"]{
  --bg:#fff5f7; --panel:#ffffff; --sunk:#ffe9ef;
  --ink:#2b1a24; --body:#55374a; --dim:#8a6b7c; --faint:#b799a8;
  --line:#f2d0dc; --line-soft:#f9e2ea;
  --accent:#d63f83; --accent-ink:#b02f6b; --accent-wash:#ffe4ee;
  --warn:#b5730a; --warn-wash:#fdf1dc; --warn-line:#ecc98f;
  --bad:#cf2b45; --bad-wash:#ffe6ea; --bad-line:#f5b3bf;
  --good:#0f8f60; --good-wash:#e2f8ee; --good-line:#a3e2c8;
  --mark:#ffe08a;
  --accent2:#e04f4f; --good2:#0f9080; --warn2:#b8571a;
  --todo-top:#fff0f5; --todo-bot:#ffffff;
  --todo-edge:#f2d0dc; --todo-bar:#d63f83; --todo-glow:#d63f83;
  --info:#3f7fd0; --info-wash:#e6eefb; --info-line:#b3cbee;
  --field-bg:#ffffff; --field-ink:#2b1a24; --field-dim:#8a6b7c;
}
*{box-sizing:border-box}
/* THE SHELL IS A SIDEBAR + A COLUMN. #shell owns the viewport as a flex ROW: a fixed-width rail
   on the left, and everything else in a column beside it. Inside that column the topbar is a
   fixed-size flex item and main is the one flexible item that soaks up whatever's left, which
   makes #list's height exact and self-adjusting - no magic "100vh minus header" number to keep in
   sync by hand, and no ambiguity between the document and #list about which one actually scrolls
   (that ambiguity is what made the wheel randomly do nothing depending on which element the
   browser picked).

   Why a rail and not the tab strip this had before: the tabs were four items that never grow,
   parked in a full-width bar that spent the other 80% of its pixels on nothing. Vertically they
   cost no horizontal room worth having on a wide screen, they can carry an icon and a count
   without crowding, and the rail gives the persistent chrome (identity at the top, the two create
   actions, connection state and theme at the very bottom) somewhere to live that isn't competing
   with the content column for the top edge. */
html,body{height:100%;overflow:hidden}
body{margin:0;background:var(--bg);color:var(--body);font:14px/1.55 var(--sans);
  -webkit-font-smoothing:antialiased}
#shell{display:flex;height:100%}
#mainwrap{flex:1;min-width:0;display:flex;flex-direction:column}
::selection{background:var(--accent-wash)}

/* ---------- sidebar ---------- */
#side{width:236px;flex:none;background:var(--panel);border-right:1px solid var(--line);
  display:flex;flex-direction:column;padding:16px 12px 12px;z-index:30}
.brand{font-size:16px;font-weight:600;letter-spacing:-.01em;color:var(--ink);
  display:flex;align-items:center;gap:10px;padding:0 6px;margin-bottom:22px}
.brand img{width:38px;height:38px;border-radius:10px;display:block;flex:none}
.brand button{background:none;border:0;padding:0;margin-left:auto;cursor:pointer;
  color:var(--faint);display:flex;align-items:center;border-radius:5px}
.brand button:hover{color:var(--dim);background:var(--sunk)}
.brand button svg{width:15px;height:15px}

/* The two ways to add something, kept side by side so neither reads as the "real" one - a memory
   and a TODO are both just a jot, so creating either is one click away from anywhere, not buried
   a tab down in Search's filter bar. Full-width in the rail: these are the only saturated fills
   on the page, which is the whole point - the eye should land on them first. */
.side-cta{display:flex;flex-direction:column;gap:7px;margin:4px 0 0;padding:0 2px}
/* .btn in the selector on purpose - a bare `.side-cta button` ties with `button.btn{width:auto}`
   on specificity and loses on source order, which is exactly how these ended up auto-width. */
.side-cta button.btn{width:100%;justify-content:center;padding:9px 12px;font-size:13px}

/* Connection state and theme, pinned to the floor of the rail. Both are things you check or
   change rarely and want out of the reading path entirely - the old header put them in the same
   band as the content headings, where they read as content. */
.side-foot{margin-top:auto;padding:12px 6px 2px;border-top:1px solid var(--line-soft);
  display:flex;flex-direction:column;gap:10px}
#palette-select{width:100%;font:11px var(--mono);color:var(--dim);background:var(--panel);
  border:1px solid var(--line);border-radius:6px;padding:6px 7px;cursor:pointer}

/* ---------- about / help ---------- */
dialog#about,dialog#agent-dialog{border:1px solid var(--line);border-radius:var(--r);padding:0;
  width:min(420px,90vw);background:var(--panel);color:var(--body);overflow:hidden}
dialog#about::backdrop,dialog#agent-dialog::backdrop{background:rgba(0,0,0,.45)}
dialog#about img{width:100%;display:block}
dialog#about .body,dialog#agent-dialog .body{padding:18px 20px 20px}
dialog#about h2,dialog#agent-dialog h2{font:600 16px var(--sans);color:var(--ink);margin:0 0 6px}
dialog#about p,dialog#agent-dialog p{margin:0 0 14px;color:var(--dim);font-size:13.5px;
  line-height:1.55}
dialog#about button.btn{width:100%}
/* Expanded from a short blurb into a real help doc - wider, and everything (the full image, not
   just a cropped strip, plus all the text) scrolls together as one document rather than the image
   being pinned as a separate banner. The dialog itself is the one scroll container.
   IMPORTANT: never put `display:` on a bare `dialog#about{...}` rule - an id selector beats the
   UA stylesheet's own `dialog:not([open]){display:none}` on specificity, which keeps the dialog
   rendered (and blocking the page) even while closed. That exact bug shipped once already. */
dialog#about{width:min(560px,92vw);max-height:80vh;overflow-y:auto}
dialog#about h3{font:600 12px var(--sans);text-transform:uppercase;letter-spacing:.05em;
  color:var(--dim);margin:18px 0 6px}
dialog#about h3:first-of-type{margin-top:4px}
dialog#about b{color:var(--ink);font-weight:600}
dialog#about code{font:11.5px var(--mono);background:var(--sunk);color:var(--accent-ink);
  padding:1px 5px;border-radius:4px}
dialog#agent-dialog{width:min(640px,92vw)}

/* ---------- history ----------
   A change is a row, not a card: you scan history looking for one moment, so the shape that helps
   is a dense uniform list with the verb and the time in fixed columns. The jot cards are for
   deciding what to read; this is for deciding what to put back. */
.hrow{display:flex;align-items:center;gap:11px;padding:9px 11px;border:1px solid var(--line);
  border-radius:8px;margin-bottom:6px;background:var(--panel)}
.hrow:hover{border-color:var(--dim)}
.hrow.sel{border-color:var(--bad);background:var(--bad-wash)}
.hop{flex:none;width:62px;font:9.5px var(--mono);text-transform:uppercase;letter-spacing:.06em;
  font-weight:700;text-align:center;padding:3px 0;border-radius:4px}
.hop.put{background:var(--accent-wash);color:var(--accent-ink)}
.hop.new{background:var(--good-wash);color:var(--good)}
.hop.del{background:var(--bad-wash);color:var(--bad)}
.hmain{flex:1;min-width:0}
.hname{font-size:13px;font-weight:600;color:var(--ink);overflow:hidden;text-overflow:ellipsis;
  white-space:nowrap}
.hname.gone{color:var(--faint);text-decoration:line-through}
.hsum{font-size:11.5px;color:var(--faint);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.hmeta{flex:none;font:10.5px var(--mono);color:var(--faint);text-align:right;min-width:104px}
.hmeta b{display:block;color:var(--dim);font-weight:400}
.hacts{flex:none;display:flex;align-items:center;gap:6px}
.hacts input[type=checkbox]{width:auto;margin:0}
.hseq{font:10px var(--mono);color:var(--faint);flex:none;min-width:34px;text-align:right}
/* The purge banner is the one thing on this page that must not be mistaken for chrome. */
.purgebanner{border:1px solid var(--bad-line);background:var(--bad-wash);border-radius:var(--r);
  padding:14px 16px;margin-bottom:16px}
.purgebanner h3{margin:0 0 6px;font-size:14px;color:var(--bad)}
.purgebanner p{margin:0 0 10px;font-size:12.5px;color:var(--body);line-height:1.5}
.purgebar{position:sticky;bottom:0;display:flex;align-items:center;gap:10px;padding:11px 14px;
  border:1px solid var(--bad-line);background:var(--bad-wash);border-radius:var(--r);
  margin-top:12px;font-size:12.5px;color:var(--body)}
.purgebar button{margin-left:auto}
dialog#purge-dialog{border:1px solid var(--line);border-radius:var(--r);padding:0;
  width:min(680px,94vw);max-height:86vh;background:var(--panel);color:var(--body);overflow-y:auto}
dialog#purge-dialog::backdrop{background:rgba(0,0,0,.5)}
dialog#purge-dialog .body{padding:18px 20px 20px}
dialog#purge-dialog h2{font:600 16px var(--sans);color:var(--ink);margin:0 0 6px}
dialog#purge-dialog p{margin:0 0 12px;color:var(--dim);font-size:13.5px;line-height:1.55}
.purgesteps{background:var(--sunk);border:1px solid var(--line);border-radius:8px;padding:12px 14px;
  font:11.5px/1.6 var(--mono);color:var(--body);white-space:pre-wrap;max-height:38vh;
  overflow-y:auto;margin-bottom:12px}

/* ---------- access list ---------- */
dialog#acl-dialog{border:1px solid var(--line);border-radius:var(--r);padding:0;
  width:min(560px,92vw);background:var(--panel);color:var(--body);overflow:hidden}
dialog#acl-dialog::backdrop{background:rgba(0,0,0,.45)}
dialog#acl-dialog .body{padding:18px 20px 20px}
dialog#acl-dialog h2{font:600 16px var(--sans);color:var(--ink);margin:0 0 6px}
dialog#acl-dialog p{margin:0 0 14px;color:var(--dim);font-size:13.5px;line-height:1.55}
.aclswitch{display:flex;align-items:center;gap:10px;padding:11px 13px;border-radius:8px;
  background:var(--sunk);margin-bottom:14px}
.aclswitch input{width:auto;margin:0;flex:none}
/* The bare `label` rule is a 10px mono all-caps field caption for the editor. This one is a
   sentence you read, so it opts out of every part of that. */
.aclswitch label{display:inline;font:600 13.5px var(--sans);color:var(--ink);cursor:pointer;
  text-transform:none;letter-spacing:normal}
.aclswitch .sub{margin-left:auto;font:11px var(--mono);color:var(--faint)}
.aclrules{display:flex;flex-direction:column;gap:6px;margin-bottom:12px;max-height:34vh;
  overflow-y:auto}
.aclrule{display:flex;align-items:center;gap:9px;padding:7px 9px;border:1px solid var(--line);
  border-radius:7px}
.aclrule .r{font:12px var(--mono);color:var(--ink);flex:none}
.aclrule .n{font-size:12px;color:var(--faint);flex:1;min-width:0;overflow:hidden;
  text-overflow:ellipsis;white-space:nowrap}
/* The rule covering the caller is marked, because "am I about to lock myself out" is the only
   question anybody actually has while looking at this list. */
.aclrule.self{border-color:var(--good-line);background:var(--good-wash)}
.aclrule .me{font:9.5px var(--mono);text-transform:uppercase;letter-spacing:.06em;
  color:var(--good);font-weight:700;flex:none}
.aclrule button{flex:none;background:none;border:0;color:var(--faint);cursor:pointer;
  font-size:15px;line-height:1;padding:0 2px}
.aclrule button:hover{color:var(--bad)}
.aclempty{color:var(--faint);font-size:12.5px;padding:10px 2px}
.aclcaller{display:flex;align-items:center;gap:8px;margin-bottom:12px;font-size:12.5px;
  color:var(--dim)}
.aclcaller b{font:12px var(--mono);color:var(--ink);font-weight:600}
.aclcaller button{margin-left:auto}
.aclerr{background:var(--bad-wash);border:1px solid var(--bad-line);color:var(--bad);
  border-radius:7px;padding:9px 11px;font-size:12.5px;line-height:1.5;margin-bottom:12px}
.aclerr .row{margin-top:9px}
.aclnote{font-size:12px;color:var(--faint);line-height:1.5;margin:0 0 14px}
dialog#agent-dialog .promptbox{max-height:min(50vh,460px);overflow-y:auto}
dialog#agent-dialog .row button{flex:1}

/* Sized for the minimal view (summary + priority/due) that's on screen by default - the full
   editor still fits fine at this width once "More fields" is open, it just isn't the width the
   dialog is optimized to look tight and clean at. */
/* The dialog element itself defaults to overflow:auto in the UA stylesheet, so with #detail
   ALSO scrolling (.pane, overflow-y:auto) the same overflowing content had two independent
   scrollbars fighting over it. overflow:hidden here leaves #detail as the one scroll container. */
/* THE DIALOG HAD NO BACKGROUND AT ALL. dialog#about sets one; this one never did, so it fell
   through to the UA stylesheet's Canvas - stark white in all twelve palettes, including the dark
   ones where it framed dark cards in a hard white border. It now takes --bg, the same ground the
   Overview panels sit on, so the dialog reads as a surface of the theme rather than a hole
   punched through it. Cards (.dsect) sit on --panel above it; the text boxes are white islands
   with dark ink, which is the one place a light surface belongs in a dark theme - a field you
   type into should look like paper whatever the app around it is doing. */
dialog#detail-dialog{border:1px solid var(--line);border-radius:var(--r);padding:0;overflow:hidden;
  width:min(640px,94vw);max-height:88vh;position:relative;
  background:var(--bg);color:var(--body);transition:width .18s ease}
/* Opening "More details" widens the dialog by half. The body of a jot is usually the longest text
   in it, and 640px was a column sized for a summary and a due date - fine until the details field
   is on screen, at which point every line wraps early. 96vw rather than 94 so the wide state can
   actually use a narrow screen instead of being clamped back to the same width. */
dialog#detail-dialog.wide{width:min(960px,96vw)}
@media(prefers-reduced-motion:reduce){dialog#detail-dialog{transition:none}}
dialog#detail-dialog::backdrop{background:rgba(0,0,0,.45)}
dialog#detail-dialog #detail{max-height:88vh;padding-right:56px}
dialog#detail-dialog .dclose{position:absolute;top:12px;right:12px;width:28px;height:28px;
  border-radius:8px;border:1px solid transparent;background:none;color:var(--dim);cursor:pointer;
  display:flex;align-items:center;justify-content:center;z-index:1}
dialog#detail-dialog .dclose svg{width:13px;height:13px}
dialog#detail-dialog .dclose:hover{background:var(--sunk);color:var(--ink)}
.brand .mk{width:15px;height:15px;flex:none;opacity:.9}
.live{display:flex;align-items:center;gap:7px;font-size:11.5px;color:var(--dim)}
.dot{width:7px;height:7px;border-radius:50%;background:var(--good);flex:none}
.dot.off{background:var(--bad)}
/* Connection state and the access list share a row at the floor of the rail. They belong together:
   both answer "can I talk to this thing, and who else can" - and the shield only means anything
   next to something that says the service is up. */
.footrow{display:flex;align-items:center;gap:8px}
.footrow #live-slot{flex:1;min-width:0}
.shieldbtn{flex:none;width:26px;height:26px;border-radius:7px;border:1px solid transparent;
  background:none;color:var(--faint);cursor:pointer;display:flex;align-items:center;
  justify-content:center;padding:0}
.shieldbtn svg{width:14px;height:14px}
.shieldbtn:hover{background:var(--sunk);color:var(--ink);border-color:var(--line)}
/* On is not a colour change alone - the icon swaps to a closed shackle. A colour-only state is one
   a colour-blind reader has to take on trust, and this is the control that decides who can reach
   the service. */
.shieldbtn.on{color:var(--good);background:var(--good-wash);border-color:var(--good-line)}

/* ---------- topbar ----------
   Deliberately thin and mostly empty. The search field is the only thing here that's used every
   session, so it gets the left edge and real width; the corpus counters sit right, quiet and
   monospaced, as a readout rather than a headline. */
#topbar{display:flex;align-items:center;gap:16px;padding:0 26px;height:60px;flex:none;
  border-bottom:1px solid var(--line);background:var(--panel);z-index:20}
.stats{margin-left:auto;display:flex;gap:18px;align-items:center}
.stat{display:flex;align-items:baseline;gap:5px}
.stat b{font:12px/1.2 var(--mono);color:var(--ink);font-weight:600}
.stat span{font-size:9.5px;text-transform:uppercase;letter-spacing:.09em;color:var(--faint)}

/* ---------- nav (vertical, in the rail) ---------- */
nav{display:flex;flex-direction:column;gap:2px;flex:none}
nav button{background:none;border:0;padding:0 10px;height:36px;cursor:pointer;width:100%;
  font:13.5px var(--sans);color:var(--dim);display:flex;align-items:center;gap:10px;
  border-radius:7px;text-align:left}
nav button .nvi{width:16px;height:16px;flex:none;display:flex;align-items:center;
  justify-content:center;color:var(--faint)}
nav button .nvi svg{width:15px;height:15px}
nav button:hover{color:var(--ink);background:var(--sunk)}
nav button.on{color:var(--accent-ink);font-weight:600;background:var(--accent-wash)}
nav button.on .nvi{color:var(--accent)}
nav .pill{margin-left:auto;font:10px var(--mono);background:var(--sunk);color:var(--dim);
  padding:1px 6px;border-radius:20px}
nav button.on .pill{background:color-mix(in srgb,var(--accent) 18%,transparent);
  color:var(--accent-ink)}

/* search lives in the topbar on every view - it stays put across tab switches */
.navsearch{position:relative;display:flex;align-items:center;flex:1;max-width:460px}
.navsearch svg{position:absolute;left:11px;width:13px;height:13px;color:var(--faint)}
.navsearch input{width:100%;font:13px var(--sans);color:var(--ink);background:var(--sunk);
  border:1px solid var(--line);border-radius:8px;height:34px;padding:0 30px}
.navsearch input::placeholder{color:var(--faint)}
.navsearch input:focus{outline:0;border-color:var(--accent);background:var(--panel);
  box-shadow:0 0 0 3px var(--accent-wash)}
.navsearch .kbd{position:absolute;right:9px;font:9.5px var(--mono);color:var(--faint);
  border:1px solid var(--line);border-radius:4px;padding:0 4px;background:var(--panel);
  pointer-events:none}

/* ---------- layout ---------- */
main{flex:1;min-height:0;overflow:hidden}
#list{height:100%}
.pane{overflow-y:auto;overscroll-behavior:contain}
#list{padding:22px 26px 64px}
/* WIDE SCREENS GET A COLUMN, NOT A STRETCH. #list stays full-bleed so the scrollbar sits at the
   window edge where it belongs, and everything inside it is capped and centered instead. Past
   ~1500px the panels stop growing: a 12-column dashboard read at 2560px wide is a row of very
   long lines and a lot of travel between related numbers, not more information. Every view builds
   into this wrapper (see render()), so no view has to remember to do it. */
.contentwrap{max-width:1500px;margin:0 auto}
/* The editor lives in dialog#detail-dialog now, not a rail that eats screen width whether or
   not anything is selected, and not a jarring reflow of the list when it opens. It always gets
   roomy space - there's no narrow-vs-editing distinction to make in a dialog. */
#detail{padding:22px 30px 40px}
.dwrap{max-width:900px}
.fld{min-width:0}
/* Priority leads the dialog: it's the field changed most often on a TODO and the one that moves
   a card between columns, so it reads as a row of visible choices rather than a menu to open.
   "Clear" is the fourth choice rather than a separate button - unset is just another state, and
   picking it deselects the other three the same way picking High does. */
/* PRIORITY IS A ROW OF TAG-SHAPED RADIOS. The native radio is still there and still does all the
   work - arrow keys walk the group, one-of-N is enforced by the browser - but it's absolutely
   positioned at zero opacity behind its own chip. That's what removes the big square focus ring
   the browser draws around a visible radio; the ring is re-attached to the chip through
   :focus-visible on the input, so keyboard focus is MORE visible than before, not less.
   Colour appears only on the chip that is set. An unselected row stays neutral grey, which means
   the single hue on screen is always the current priority - the same red/blue/grey the cards and
   column headers use, so the control and its consequence match. */
.prio{display:flex;gap:7px;flex-wrap:wrap}
.prchip{position:relative;display:inline-flex;margin:0}
.prchip input{position:absolute;inset:0;width:100%;height:100%;opacity:0;margin:0;cursor:pointer}
.prchip span{font:10.5px var(--mono);text-transform:uppercase;letter-spacing:.07em;font-weight:600;
  padding:6px 12px;border-radius:6px;border:1px solid var(--line);background:var(--sunk);
  color:var(--dim);user-select:none}
.prchip:hover span{color:var(--ink);border-color:var(--dim)}
.prchip input:focus-visible+span{outline:2px solid var(--accent);outline-offset:2px}
.prchip input:checked+span{color:var(--panel);border-color:transparent;
  box-shadow:inset 0 1px 0 rgba(255,255,255,.22)}
.prchip.p-high input:checked+span{background:var(--bad)}
.prchip.p-normal input:checked+span{background:var(--info)}
.prchip.p-low input:checked+span{background:var(--dim)}
/* "Clear" is the absence of a priority, so it gets no hue - just an outline saying it's the one
   that's set. A colour here would imply it were a fourth priority level. */
.prchip.p-none input:checked+span{background:none;color:var(--ink);border-color:var(--dim);
  box-shadow:none}

/* SECTIONS ARE ENCLOSURES, not just gaps. The dialog grew past the point where whitespace alone
   said which fields belong together, so each group is a bordered box with a legend and the
   collapse control sits in the bottom-right of the box it belongs to. */
/* The card is --panel lifted by a flat 8% white wash. On a light palette --panel is already white
   so the wash does nothing and the separation comes from the tinted ground underneath; on a dark
   one, where --bg and --panel are only a shade apart, it's what makes the card actually read as a
   card. One declaration that does the right thing at both ends. */
.dsect{border:1px solid var(--line);border-radius:9px;padding:12px 15px 14px;margin-bottom:13px;
  background:linear-gradient(rgba(255,255,255,.08),rgba(255,255,255,.08)),var(--panel);
  box-shadow:0 1px 3px rgba(0,0,0,.14)}
/* Legends name the sections, so they have to survive a dark palette. At 10px in --faint they did
   not - --faint is the tone for things you are meant to skip past. Bigger, bolder, and in --body,
   which is the same tone the prose uses. Not scoped to .dsect: priority has no box any more and
   still needs its heading. */
.dlegend{font:11.5px var(--mono);text-transform:uppercase;letter-spacing:.09em;font-weight:700;
  color:var(--body);margin-bottom:11px}
/* The field labels a level below the legends had the same problem for the same reason: --faint
   measured 1.55:1 on a Nord Dark card. --dim keeps them clearly subordinate to a legend while
   staying legible. Scoped to the dialog - elsewhere --faint labels sit on different grounds. */
#detail label{color:var(--dim)}
/* Priority is a row of chips that already read as a group - a box around them was one enclosure
   too many, and the chips are self-labelling in a way a form field is not. */
.dbare{margin-bottom:15px}
/* Labels inside a section carry no top margin - the section legend and the row spacing below
   provide it. Leaving the global 14px on meant the two halves of a .frow started at different
   heights whenever only one of them was the row's first child. */
.dsect .fld>label:first-child{margin-top:0}
.dsect .frow{margin-top:15px}
.dsect .frow:first-of-type{margin-top:0}
.dsect textarea,.dsect input{width:100%}
.dsectfoot{display:flex;justify-content:flex-end;margin-top:10px}
.dmktodo{margin-bottom:13px}
/* A class that sets `display` outsmarts the UA rule for the hidden attribute, which is how the
   collapsed dialog kept showing the toggle it had just hidden. */
[hidden]{display:none!important}
/* The selected priority fills with the same accent ramp the primary button uses - a TODO's
   priority is the thing that colors its whole card, so the control that sets it should look
   like a decision, not like a checkbox that happens to be ticked. */
/* Due and its snooze buttons are a subsection of their own now that priority has left the row -
   scheduling is a separate decision from how much the thing matters. */
.dsect input.dueinput{max-width:260px}
.frow{display:grid;grid-template-columns:1fr 1fr;gap:0 18px}
@media(max-width:640px){.frow{grid-template-columns:1fr}}
#detail textarea[data-k=text]{min-height:min(30vh,320px)}
#detail textarea[data-k=summary]{min-height:92px}
.snbtns{display:flex;gap:6px;flex-wrap:wrap;margin:10px 0 16px}

/* ---------- controls ---------- */
.search{position:relative;margin-bottom:12px}
.search input{width:100%;font:15px var(--sans);color:var(--ink);background:var(--panel);
  border:1px solid var(--line);border-radius:var(--r);padding:10px 12px 10px 34px}
.search svg{position:absolute;left:11px;top:11px;width:14px;height:14px;color:var(--faint)}
.search input:focus{outline:0;border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-wash)}
.search input::placeholder{color:var(--faint)}
.kbd{position:absolute;right:10px;top:9px;font:10px var(--mono);color:var(--faint);
  border:1px solid var(--line);border-radius:4px;padding:1px 5px;background:var(--sunk)}

/* ---------- filter bar (Search view: sort, time range) ---------- */
.filterbar{display:flex;align-items:center;gap:14px;flex-wrap:wrap;margin-bottom:12px}
.fgroup{display:flex;align-items:center;gap:7px}
.flabel{font:10px var(--mono);text-transform:uppercase;letter-spacing:.06em;color:var(--faint)}
.toggle{width:30px;height:17px;border-radius:20px;background:var(--sunk);border:1px solid var(--line);
  position:relative;flex:none;cursor:pointer}
.toggle i{position:absolute;top:1px;left:1px;width:13px;height:13px;border-radius:50%;
  background:var(--faint);transition:left .12s}
.toggle.on{background:var(--accent-wash);border-color:var(--accent)}
.toggle.on i{left:14px;background:var(--accent)}

input,select,textarea{font:14px var(--sans);color:var(--ink);background:var(--panel);
  border:1px solid var(--line);border-radius:6px;padding:7px 9px;width:100%}
textarea{resize:vertical;line-height:1.55}
input:focus,select:focus,textarea:focus{outline:0;border-color:var(--accent);
  box-shadow:0 0 0 3px var(--accent-wash)}

/* Text boxes in the editor are paper in every palette: a light ground with dark ink, because a
   field you type into should look like something you type into whatever the app around it is
   doing. On a DARK palette that ground is a light tint of the theme's own hue, not pure white -
   white won the contrast argument and lost the comfort one, a floodlight in a dark room. The tint
   sits near 72% of white's luminance and still prints ink above 10:1.
   color-scheme:light rides along so the native parts a dark palette would otherwise render dark -
   the datetime picker's calendar button and its dropdown, the caret - come out matching the light
   field instead of the dark page. The radios are excluded: they're the invisible ones behind the
   priority chips, and a light fill on them would defeat the chip. */
#detail input:not([type=radio]),#detail textarea{
  background:var(--field-bg);color:var(--field-ink);color-scheme:light}
#detail input:not([type=radio])::placeholder,#detail textarea::placeholder{color:var(--field-dim)}

/* Even the plain button gets a ramp - panel at the top, sunk at the bottom. It's a two-value
   difference in every palette, so it costs nothing and stops a row of outline buttons from
   reading as flat rectangles next to the filled ones. */
button.btn{font:13px var(--sans);color:var(--ink);cursor:pointer;
  background:linear-gradient(180deg,var(--panel),var(--sunk));
  border:1px solid var(--line);border-radius:6px;padding:7px 12px;width:auto}
button.btn:hover{border-color:var(--dim);box-shadow:0 2px 6px rgba(0,0,0,.10)}
button.btn:active{box-shadow:inset 0 2px 4px rgba(0,0,0,.14)}

/* ---------- FILLED CONTROLS ----------
   One recipe, shared by every filled control in the app: a two-stop ramp from the lighter second
   stop at the top to the base color at the bottom, a hairline inset highlight along that top edge
   so the surface reads as lit from above, and on hover a drop shadow carrying the control's OWN
   hue rather than generic black.
   Text is var(--panel), not #fff. On a dark palette the accent IS the light color, so white-on-
   accent was the low-contrast pairing; panel tracks the page background, which makes it dark text
   on a dark theme's bright button and white text on a light theme's deep one - right in both
   directions without a per-palette token to maintain.
   The hue-tinted glow is the only color-mix() in the file and it sits behind @supports on purpose:
   a browser without it loses the glow and keeps everything else. The gradients themselves stick to
   plain declared tokens, because a var() that fails to substitute inside `background` does not
   fall back to the previous declaration - it unsets the property, and an invisible Save button is
   a far worse failure than a missing shadow. */
button.primary,button.warnfill,button.badfill,.chip.on,.completebtn:not(.on),.prio label.on{
  color:var(--panel);border-style:solid;border-width:1px;
  box-shadow:0 1px 2px rgba(0,0,0,.16),inset 0 1px 0 rgba(255,255,255,.22)}
button.primary:hover,button.warnfill:hover,button.badfill:hover,.completebtn:not(.on):hover{
  box-shadow:0 3px 12px rgba(0,0,0,.22),inset 0 1px 0 rgba(255,255,255,.3)}
button.primary:active,button.warnfill:active,button.badfill:active,.completebtn:not(.on):active{
  filter:brightness(.96);box-shadow:inset 0 2px 5px rgba(0,0,0,.3)}

button.primary,.chip.on,.prio label.on{
  background:linear-gradient(170deg,var(--accent2),var(--accent));border-color:var(--accent)}
button.primary{font-weight:600}
button.primary:hover{filter:brightness(1.05) saturate(1.08);border-color:var(--accent)}
/* Same recipe tinted --warn instead of --accent - reuses the color the TODO panel already uses for
   "this needs action" rather than inventing a second accent. */
button.warnfill{background:linear-gradient(var(--cta-ang),var(--cta-a),var(--cta-b));
  border-color:var(--cta-edge);font-weight:600}
button.warnfill:hover{filter:brightness(1.05) saturate(1.08);border-color:var(--cta-edge)}
/* The same recipe again, tinted --bad. EXACTLY ONE control in the app wears this - the purge - and
   that is the point: an irreversible action should not be wearing the same clothes as Save. It is a
   class on the shared recipe rather than a rule on one id so the filled buttons stay siblings. */
button.badfill{background:linear-gradient(170deg,var(--bad2),var(--bad));
  border-color:var(--bad);font-weight:600}
button.badfill:hover{filter:brightness(1.05) saturate(1.08);border-color:var(--bad)}

@supports (color:color-mix(in srgb,red,blue)){
  button.primary:hover{box-shadow:0 3px 14px color-mix(in srgb,var(--accent) 45%,transparent),
    inset 0 1px 0 rgba(255,255,255,.3)}
  button.warnfill:hover{box-shadow:0 3px 14px color-mix(in srgb,var(--cta-b) 45%,transparent),
    inset 0 1px 0 rgba(255,255,255,.3)}
  button.badfill:hover{box-shadow:0 3px 14px color-mix(in srgb,var(--bad) 45%,transparent),
    inset 0 1px 0 rgba(255,255,255,.3)}
  .completebtn:not(.on):hover{box-shadow:0 3px 14px color-mix(in srgb,var(--good) 45%,transparent),
    inset 0 1px 0 rgba(255,255,255,.3)}
}

/* Ghost opts back OUT of all of it - it's the "this is not the action you came for" button. */
button.ghost{border-color:transparent;color:var(--dim);background:none;box-shadow:none}
button.ghost:hover{color:var(--ink);border-color:var(--line);background:var(--sunk);box-shadow:none}
button.danger:hover{border-color:var(--bad);color:var(--bad)}
button.tiny{font-size:12px;padding:4px 9px}
#notif-btn{display:flex;align-items:center;gap:6px}
#notif-btn svg{width:13px;height:13px;flex:none}
#notif-btn.on{color:var(--good);border-color:var(--good-line);background:var(--good-wash)}
#notif-btn.off{opacity:.6}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}

/* ---------- filter chips ---------- */
.chips{display:flex;gap:5px;flex-wrap:wrap;margin-bottom:14px}
.chip{border:1px solid var(--line);border-radius:20px;padding:3px 10px;font-size:12px;
  cursor:pointer;color:var(--dim);background:var(--panel);white-space:nowrap;user-select:none}
.chip:hover{border-color:var(--dim);color:var(--ink)}
.chip.on{font-weight:500}
.chip b{font:10px var(--mono);opacity:.65;margin-left:5px;font-weight:400}
/* The x is only drawn on an active chip, so it reads as "remove this filter" rather than as
   decoration on every tag in the row. */
.chip .x{font-style:normal;font-size:13px;line-height:1;margin-left:6px;opacity:.75}
.chip.clearall{border-style:dashed;color:var(--faint)}
.chip.clearall:hover{color:var(--bad);border-color:var(--bad-line)}

/* ---------- result meta ---------- */
.rmeta{display:flex;align-items:baseline;gap:8px;margin:2px 0 12px;
  font:11px var(--mono);color:var(--faint);text-transform:uppercase;letter-spacing:.06em}
.rmeta em{font-style:normal;color:var(--dim)}
.daybar{position:sticky;top:0;z-index:5;background:var(--bg);padding:10px 0 6px;
  font:11px var(--mono);color:var(--faint);text-transform:uppercase;letter-spacing:.08em;
  border-bottom:1px solid var(--line-soft);margin-bottom:8px}

/* ---------- jot cards ----------
   Cards, not rows: ranking is still visible (the accent top-edge + score badge carry what the
   rail used to), but a jot no longer gets to sprawl into a wall of text - headline and preview
   both clamp to two lines. The top-edge color is the tag-derived "category" - see catColorOf(). */
mark{background:var(--mark);color:inherit;border-radius:2px;padding:0 1px}
.tag{background:var(--tag-bg);color:var(--tag-ink);border-radius:4px;padding:1px 6px;font-size:11px}
.tag.res{background:transparent;border:1px dashed var(--line);color:var(--faint)}
/* Action tags (todo/warning/error) get a fixed, non-hashed color - unlike catColorOf()'s
   category dot, these mean "you may need to act on this" regardless of topic, so they always
   read the same way. Reuses --bad, which CAT_VARS deliberately never hashes onto, so this can
   never collide with a category color. */
.tag.action{background:var(--bad-wash);color:var(--bad);border:1px solid var(--bad-line)}

/* Airy on purpose - fewer, bigger cards read faster than many cramped ones. Widened the column
   floor, and gave every gap and edge more room, rather than fitting more per row. */
.cardgrid{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:14px}
.mcard{position:relative;background:var(--panel);border:1px solid var(--line);border-radius:var(--r);
  border-top:3px solid var(--cat,var(--accent));padding:16px 17px 14px;cursor:pointer;
  display:flex;flex-direction:column;gap:9px;min-height:200px}
.mcard:hover{border-color:var(--dim)}
.mcard.sel{border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-wash)}
.cathead{display:flex;align-items:center;gap:7px;min-width:0}
/* A tinted chip, not a dot-plus-caption - the category has to read at a glance, the way the
   screenshot's scope badge does, not require parsing a line of small caps. color-mix keeps this
   working across every category (only 4 hues in CAT_VARS) and every palette without a matching
   --cat-wash variable for each. */
.catpill{flex:none;background:color-mix(in srgb,var(--cat,var(--accent)) 16%,transparent);
  color:var(--cat,var(--accent));font:10px var(--mono);text-transform:uppercase;
  letter-spacing:.05em;padding:2px 7px;border-radius:20px}
/* The slug is the fast-recognition handle for a named jot - short, stable, chosen on purpose - so
   it leads in the big type. Everything else is one quiet, single-clamped line underneath. */
.title{color:var(--ink);font-size:15.5px;font-weight:600;line-height:1.3;letter-spacing:-.005em;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.headline{color:var(--dim);font-size:12.5px;line-height:1.5;
  display:-webkit-box;-webkit-line-clamp:7;-webkit-box-orient:vertical;overflow:hidden}
.mfoot{margin-top:auto;display:flex;align-items:center;gap:6px;flex-wrap:wrap;
  font:11px var(--mono);color:var(--faint)}
.mtrail{margin-left:auto;display:flex;gap:6px;align-items:center}
.scbadge{font:10.5px var(--mono);color:var(--accent-ink);font-weight:600}

/* view toggle - cards (the default grid above) vs. a dense wide list, same .mcard markup either
   way. List mode just reflows the card's own children into a row instead of adding new markup. */
.viewtoggle{display:flex;border:1px solid var(--line);border-radius:7px;overflow:hidden}
.viewtoggle button{background:var(--panel);border:0;padding:5px 8px;cursor:pointer;color:var(--dim);
  display:flex}
.viewtoggle button+button{border-left:1px solid var(--line)}
.viewtoggle button svg{width:13px;height:13px}
.viewtoggle button.on{background:var(--accent-wash);color:var(--accent-ink)}
.viewtoggle button:hover{color:var(--ink)}
.cardgrid.list{display:flex;flex-direction:column;gap:6px}
.cardgrid.list .mcard{flex-direction:row;align-items:center;gap:14px;min-height:auto;
  padding:9px 14px;border-top:1px solid var(--line);border-left:3px solid var(--cat,var(--accent))}
.cardgrid.list .cathead{flex:0 0 100px}
.cardgrid.list .title{flex:0 0 210px}
.cardgrid.list .headline{flex:1 1 auto;-webkit-line-clamp:1;white-space:nowrap;text-overflow:ellipsis}
.cardgrid.list .mfoot{margin-top:0;flex:0 0 auto;flex-wrap:nowrap}

/* ---------- dashboard: overview ----------
   Four stat cards, then two rows of two panels - distribution/signals, activity/health. Nothing
   here needs its own page: it's what you'd want visible before deciding where to dig in, which is
   also why the periodic refresh (see viewDashboard) keeps it live without a manual reload. */
.ov-grid{display:grid;grid-template-columns:1.3fr 1fr 1fr 1fr;gap:12px;margin-bottom:20px}
@media(max-width:900px){.ov-grid{grid-template-columns:repeat(2,1fr)}}
.ov-card{background:var(--panel);border:1px solid var(--line);border-radius:var(--r);
  padding:16px 18px;display:flex;flex-direction:column;gap:9px;min-height:100px}
.ov-card .top{display:flex;align-items:center;gap:10px}
.ov-card .ic{width:28px;height:28px;border-radius:8px;background:var(--sunk);color:var(--dim);
  display:flex;align-items:center;justify-content:center;flex:none}
.ov-card .ic svg{width:14px;height:14px}
/* Not .ov-card .eyebrow - the panel kickers (DISTRIBUTION, ACTIVITY, STORE HEALTH, TODOS &
   REMINDERS) carry the same class and were matching nothing, so they rendered as plain 14px body
   text next to the 10px mono ones on the stat cards. One rule, both places. */
.eyebrow{font:10px var(--mono);text-transform:uppercase;letter-spacing:.07em;
  color:var(--eyebrow-ink)}
.ov-card .num{font-size:26px;font-weight:600;color:var(--ink);line-height:1}
.ov-card .cap{font-size:11.5px;color:var(--faint)}
.ov-card.hi{background:linear-gradient(var(--hi-ang),var(--hi-a),var(--hi-b));
  border-color:var(--hi-a)}
.ov-card.hi .ic{background:rgba(255,255,255,.2);color:#fff}
.ov-card.hi .eyebrow,.ov-card.hi .cap{color:rgba(255,255,255,.78)}
.ov-card.hi .num{color:#fff}

/* align-items:start so a panel is only as tall as its content - a 10-row distribution chart next
   to an 8-pill tag cloud was stretching the tag panel to match and leaving half of it blank. */
.ov-row{display:grid;grid-template-columns:1.7fr 1fr;gap:12px;margin-bottom:12px;align-items:start}
@media(max-width:900px){.ov-row{grid-template-columns:1fr}}
.ov-panel{background:var(--panel);border:1px solid var(--line);border-radius:var(--r);
  padding:16px 18px}
.ov-panel .phead{display:flex;align-items:baseline;justify-content:space-between;margin-bottom:14px}
.ov-panel .phead h3{margin:2px 0 0;font-size:14px;font-weight:600;color:var(--ink)}
.ov-panel .phead a{font-size:12px;color:var(--accent-ink);cursor:pointer}
/* The TODO panel is the one thing on the dashboard meant to interrupt you, so it doesn't get to
   look like just another ov-panel - tinted ground, a bolder rule under the eyebrow, a visible
   accent bar down the left edge. Everything else on Overview is context; this is the thing to
   act on, and it should read that way before you've read a word of it.
   THE GROUND IS A TOKEN, not a hardcoded --warn-wash, because "tint the whole surface with the
   warning color" only works when the warning color is a pale cream. On a saturated dark palette
   that same rule paints a large olive-brown slab underneath violet cards, which is exactly as
   good as it sounds. The default below is a flat warn wash - a gradient between two identical
   stops, so every palette that doesn't override renders byte-identically to before - and the
   vivid palettes redefine the four tokens to get a different treatment entirely. */
.ov-todo{margin-bottom:20px;border-color:var(--todo-edge);
  background:linear-gradient(180deg,var(--todo-top),var(--todo-bot));
  border-left:4px solid var(--todo-bar);padding-left:17px;
  box-shadow:0 3px 16px -6px color-mix(in srgb,var(--todo-glow) 55%,transparent)}
/* margin-right:auto on the title block, not space-between on the parent - with three children
   (badge, titles, link) space-between stranded the heading floating in the middle of the panel
   instead of reading as a caption on the badge beside it. */
.ov-todo .phead{align-items:center;gap:12px}
.phead .pheadmain{margin-right:auto}
.ov-todo .phead .eyebrow{color:var(--warn);font-weight:700}
.ov-todo .phead h3{font-size:17px}
.ov-todobadge{flex:none;width:34px;height:34px;border-radius:9px;background:var(--warn);color:#fff;
  display:flex;align-items:center;justify-content:center;box-shadow:0 2px 8px -2px color-mix(in srgb,var(--warn) 70%,transparent)}
.ov-todobadge svg{width:16px;height:16px}
.ov-todocols{display:grid;grid-template-columns:repeat(3,1fr);gap:18px}
@media(max-width:760px){.ov-todocols{grid-template-columns:1fr}}
.ov-todocolhead{display:flex;align-items:center;gap:7px;margin-bottom:9px;padding-bottom:7px;
  border-bottom:2px solid var(--line)}
.ov-todocol.pr-high .ov-todocolhead{border-bottom-color:var(--bad)}
.ov-todocol.pr-normal .ov-todocolhead{border-bottom-color:var(--warn)}
.ov-todocol.pr-low .ov-todocolhead{border-bottom-color:var(--line-soft)}
.ov-todocolhead .ptitle{font-size:11.5px;font-weight:700;color:var(--ink);text-transform:uppercase;
  letter-spacing:.06em}
.ov-todocol.pr-high .ptitle{color:var(--bad)}
.ov-todocol.pr-normal .ptitle{color:var(--warn)}
.ov-todocolhead .pcount{margin-left:auto;font:11px var(--mono);color:var(--faint)}
.ov-todobody{min-height:34px;border-radius:8px;transition:background .1s ease}
.ov-todobody.dragover{background:var(--sunk);outline:2px dashed var(--dim);outline-offset:-2px}
/* A TODO IS A CARD YOU CAN ACT ON WITHOUT OPENING IT.
   The old row was a checkbox, one bold line and one faint line - to find out what a TODO actually
   was, or to do anything but complete it, you had to open the dialog. The card states the four
   things you need to triage at a glance, in reading order:

     1. what kind of thing it is      - the tag chips across the top (TODO / priority / scope)
     2. what it is                    - the title, in real type
     3. when it's due                 - its own line, red and bold once that time is past
     4. what you can do about it      - Edit / Snooze / Complete, visible, not hover-revealed

   Hidden controls were the wrong call here: this panel exists to be dispatched, and a button you
   have to discover by hovering is a button you don't count on. Dragging between columns still
   reprioritizes, so the card is a drag handle too - hence the explicit draggable=false on every
   button inside it. */
.ov-trow{display:flex;flex-direction:column;gap:7px;background:var(--panel);
  border:1px solid var(--line);border-left:3px solid var(--dim);border-radius:8px;
  padding:11px 13px 10px;margin-bottom:9px;cursor:grab}
.ov-trow:active{cursor:grabbing}
.ov-trow:hover{border-color:var(--dim);box-shadow:0 2px 10px -3px rgba(0,0,0,.13)}
.ov-trow.dragging{opacity:.35}
.ov-todocol.pr-high .ov-trow{border-left-color:var(--bad)}
.ov-todocol.pr-normal .ov-trow{border-left-color:var(--warn)}

/* chip row - the "what kind of thing is this" line the reference leads with */
.ov-tchips{display:flex;align-items:center;gap:5px;flex-wrap:wrap}
.ov-tchip{font:9.5px var(--mono);text-transform:uppercase;letter-spacing:.06em;font-weight:600;
  padding:2px 6px;border-radius:4px;background:var(--tag-bg);color:var(--tag-ink);flex:none}
.ov-tchip.todo{background:var(--warn-wash);color:var(--warn);border:1px solid var(--warn-line)}
.ov-tchip.pr-high{background:var(--bad-wash);color:var(--bad);border:1px solid var(--bad-line)}
.ov-tchip.pr-normal{background:var(--tag-bg);color:var(--tag-ink)}
.ov-tchip.pr-low{background:var(--tag-bg);color:var(--faint)}
/* the scope/topic chip carries the same hashed category color the rest of the UI uses */
.ov-tchip.scope{background:color-mix(in srgb,var(--cat,var(--accent)) 15%,transparent);
  color:var(--cat,var(--accent));text-transform:none;letter-spacing:.02em}
.ov-tid{margin-left:auto;font:10px var(--mono);color:var(--faint)}

.ov-trow .ov-atitle{font-size:13.5px;font-weight:600;white-space:normal;line-height:1.35;
  color:var(--ink)}
/* Due reads as a full absolute stamp, not just "Overdue by 3 days" - when triaging you want to
   know it was due Tuesday at 9, not do the arithmetic back from a relative figure. The relative
   magnitude rides along in parentheses since it's the faster of the two to compare. */
.ov-tdue{font-size:11.5px;color:var(--dim);display:flex;align-items:center;gap:6px}
.ov-tdue svg{width:12px;height:12px;flex:none;opacity:.75}
.ov-tdue.over{color:var(--bad);font-weight:600}
.ov-tdue .rel{color:var(--faint);font-weight:400}
.ov-tdue.over .rel{color:var(--bad);opacity:.75}

.ov-tacts{display:flex;align-items:center;gap:6px;margin-top:2px;padding-top:8px;
  border-top:1px solid var(--line-soft)}
.ov-tacts button:not(.completebtn){font:11.5px var(--sans);color:var(--dim);background:none;
  cursor:pointer;border:1px solid transparent;border-radius:6px;padding:3px 8px;display:flex;
  align-items:center;gap:5px}
.ov-tacts button:not(.completebtn):hover{color:var(--ink);border-color:var(--line);
  background:var(--sunk)}
.ov-tacts button svg{width:11px;height:11px;flex:none}

/* ONE Complete treatment, shared by the reminder card and the detail dialog: same green, same
   box-and-check, same caption, and margin-left:auto puts it bottom-right in both. Completing a
   TODO is then one gesture to learn, not two that happen to do the same thing. It is also the
   only affirmative action in either place, which is why it is the only one carrying color -
   and on the card it sits at the far end, away from Edit/Snooze on something you may be dragging. */
.completebtn{margin-left:auto;display:inline-flex;align-items:center;gap:6px;cursor:pointer;
  font:12px var(--sans);font-weight:600;border-radius:6px;padding:5px 11px;
  background:linear-gradient(170deg,var(--good2),var(--good));border-color:var(--good)}
.completebtn:hover{filter:brightness(1.05) saturate(1.08);border-color:var(--good)}
.completebtn svg{width:13px;height:13px;flex:none}
/* Already completed: the box reads as ticked, and the button becomes the way back. */
.completebtn.on{color:var(--dim);background:var(--sunk);border:1px solid var(--line);
  font-weight:400;box-shadow:none}
.completebtn.on:hover{color:var(--ink);background:var(--sunk);border-color:var(--dim)}
.ov-tacts .completebtn{font-size:11.5px;padding:3px 9px}
.ov-tacts .completebtn svg{width:12px;height:12px}
.ov-colempty{color:var(--faint);font-size:12px;padding:8px 0}
.ov-asub.bad{color:var(--bad)}

.ov-bar{display:flex;align-items:center;gap:10px;margin-bottom:10px}
.ov-bar:last-child{margin-bottom:0}
.ov-bar .lbl{width:112px;flex:none;font-size:12.5px;color:var(--body);white-space:nowrap;
  overflow:hidden;text-overflow:ellipsis}
.ov-bar .track{flex:1;height:8px;border-radius:4px;background:var(--sunk);overflow:hidden}
.ov-bar .fill{display:block;height:100%;border-radius:4px;background:var(--cat,var(--accent))}
.ov-bar .n{width:26px;text-align:right;font:11px var(--mono);color:var(--dim)}

.ov-tags{display:flex;flex-wrap:wrap;gap:8px}
.ov-tagpill{background:var(--tag-bg);border:1px solid var(--line);border-radius:20px;
  padding:5px 10px;font-size:12px;color:var(--tag-ink);display:flex;align-items:center;gap:5px;
  cursor:pointer}
.ov-tagpill:hover{border-color:var(--dim);color:var(--ink)}
.ov-tagpill b{font:10px var(--mono);color:var(--dim);font-weight:600}

.ov-activity{display:grid;grid-template-columns:1fr 1fr;gap:0 20px}
@media(max-width:900px){.ov-activity{grid-template-columns:1fr}}
.ov-arow{display:flex;align-items:center;gap:9px;padding:8px 0;cursor:pointer;
  border-bottom:1px solid var(--line-soft)}
.ov-arow:hover .ov-atitle{color:var(--accent-ink)}
.ov-adot{width:7px;height:7px;border-radius:50%;background:var(--cat,var(--accent));flex:none}
.ov-amid{min-width:0;flex:1}
.ov-atitle{font-size:12.5px;color:var(--ink);font-weight:500;white-space:nowrap;
  overflow:hidden;text-overflow:ellipsis}
.ov-asub{font-size:11px;color:var(--faint);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.ov-awhen{margin-left:auto;font:10.5px var(--mono);color:var(--faint);flex:none}

.taskbits{display:flex;gap:6px;align-items:center}
.priochip{font:10px var(--mono);text-transform:uppercase;letter-spacing:.04em;padding:2px 6px;
  border-radius:4px}
.priochip.pr-high{background:var(--bad-wash);color:var(--bad)}
.priochip.pr-low{background:var(--sunk);color:var(--faint)}
.duechip{font:10.5px var(--mono);color:var(--faint);padding:2px 6px;border-radius:4px;
  background:var(--sunk)}
.duechip.over{background:var(--bad-wash);color:var(--bad);font-weight:600}
.donechip{font:10px var(--mono);text-transform:uppercase;letter-spacing:.04em;padding:2px 6px;
  border-radius:4px;background:var(--good-wash);color:var(--good)}
.mcard.done{opacity:.6}
.mcard.done .title{text-decoration:line-through;text-decoration-color:var(--faint)}

.ov-health .lead{font-size:12.5px;color:var(--good);margin-bottom:12px;line-height:1.5}
.ov-health .vrow{cursor:default;padding:6px 0}
.ov-health .vrow:hover{background:transparent}

/* the copy-into-a-fresh-agent-session prompt - a preview you can read, not a wall to scroll */
.promptbox{border:1px solid var(--line);background:var(--sunk);border-radius:var(--r);
  padding:11px 13px;font:11.5px/1.65 var(--mono);color:var(--body);white-space:pre-wrap;
  max-height:150px;overflow:hidden;position:relative;transition:max-height .15s ease}
.promptbox.open{max-height:none}
.promptbox:not(.open)::after{content:"";position:absolute;left:0;right:0;bottom:0;height:44px;
  background:linear-gradient(transparent,var(--sunk))}
.colhead{display:flex;align-items:baseline;gap:8px;margin:22px 0 11px}
.colhead:first-child{margin-top:0}
.colhead h2{margin:0;font-size:13px;font-weight:600;color:var(--ink)}
.colhead span{font-size:11.5px;color:var(--faint)}

/* ---------- notes / empty ---------- */
.empty{text-align:center;padding:44px 20px;color:var(--faint)}
.empty b{display:block;color:var(--dim);font-size:14px;font-weight:500;margin-bottom:5px}
.note{border:1px solid var(--line);border-left:3px solid var(--dim);background:var(--panel);
  padding:9px 12px;border-radius:6px;margin-bottom:10px;font-size:13px;color:var(--body)}
.note.warn{border-left-color:var(--warn);background:var(--warn-wash);border-color:var(--warn-line);
  color:var(--warn)}
.note.good{border-left-color:var(--good);background:var(--good-wash);border-color:var(--good-line);
  color:var(--good)}
.note.bad{border-left-color:var(--bad);background:var(--bad-wash);border-color:var(--bad-line);
  color:var(--bad)}

/* ---------- tags view ---------- */
.sect{font:11px var(--mono);color:var(--faint);text-transform:uppercase;letter-spacing:.08em;
  margin:22px 0 9px}
.sect:first-child{margin-top:2px}
.cluster{border:1px solid var(--warn-line);background:var(--warn-wash);border-radius:var(--r);
  padding:12px 13px;margin-bottom:8px}
.cluster .lead{font-size:12.5px;color:var(--warn);margin-bottom:9px}
.vrow{display:flex;align-items:center;gap:10px;padding:6px 9px;border-radius:5px;
  cursor:pointer;position:relative;overflow:hidden}
.vrow:hover{background:var(--sunk)}
.vrow .fill{position:absolute;left:0;top:0;bottom:0;background:var(--accent-wash);z-index:0}
.vrow > *{position:relative;z-index:1}
.vrow .n{font:11px var(--mono);color:var(--dim);margin-left:auto}
.vrow .when{font:10px var(--mono);color:var(--faint);width:70px;text-align:right}

/* ---------- detail ---------- */
/* Slug left, id right: the id is the thing you occasionally need to quote and never need to read,
   so it sits at the far end in mono at the size of a footnote. */
.dhead{display:flex;align-items:baseline;gap:12px;margin-bottom:14px}
.dhead h3{margin:0;font-size:16px;color:var(--ink);font-weight:600;overflow-wrap:anywhere}
.dhead .did{margin-left:auto;font:10.5px var(--mono);color:var(--faint);flex:none}
.dmeta{font:11px var(--mono);color:var(--faint);line-height:1.7;margin-top:12px;
  padding-top:11px;border-top:1px solid var(--line-soft)}
.dmeta div{display:flex;gap:8px}
.dmeta i{font-style:normal;color:var(--dim);width:62px;flex:none}
label{display:block;font:10px var(--mono);text-transform:uppercase;letter-spacing:.08em;
  color:var(--faint);margin:14px 0 5px}
label u{text-decoration:none;color:var(--accent-ink);text-transform:none;letter-spacing:0;
  font-family:var(--sans);font-size:11px;margin-left:6px}
.actions{display:flex;gap:8px;margin-top:20px;padding-top:14px;
  border-top:1px solid var(--line-soft);flex-wrap:wrap}

/* ---------- toast ---------- */
#toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%) translateY(8px);z-index:60;
  padding:10px 16px;border-radius:8px;font-size:13px;max-width:min(520px,90vw);
  box-shadow:0 6px 24px rgba(0,0,0,.16);opacity:0;pointer-events:none;
  display:flex;align-items:center;gap:12px;transition:opacity .16s,transform .16s}
#toast.show{opacity:1;transform:translateX(-50%) translateY(0);pointer-events:auto}
#toast.ok{background:var(--good-wash);color:var(--good);border:1px solid var(--good-line)}
#toast.err{background:var(--bad-wash);color:var(--bad);border:1px solid var(--bad-line)}
#toast.warn{background:var(--warn-wash);color:var(--warn);border:1px solid var(--warn-line)}
#toast .undo{flex:none;font:inherit;font-weight:700;text-decoration:underline;background:none;
  border:0;padding:0;cursor:pointer;color:inherit}
</style>
</head>
<body>

<div id="shell">

  <aside id="side">
    <div class="brand">
      <img src="/icon.png" width="38" height="38" alt="">
      Loom
      <button type="button" id="about-btn" title="Help" aria-label="Help">
        <svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">
          <circle cx="8" cy="8" r="6.3"/><path d="M6.1 6.3a2 2 0 0 1 3.8.7c0 1.3-1.9 1.5-1.9 2.9"/><path d="M8 12v.1"/>
        </svg>
      </button>
    </div>

    <nav id="nav"></nav>

    <div class="side-cta">
      <button type="button" class="btn primary" id="new-jot-btn">+ New Jot</button>
      <button type="button" class="btn warnfill" id="new-todo-btn">+ New TODO</button>
    </div>

    <div class="side-foot">
      <div class="footrow">
        <div id="live-slot"></div>
        <button type="button" class="shieldbtn" id="acl-btn" title="Access list">
          <svg id="acl-icon" viewBox="0 0 16 16" fill="none" stroke="currentColor"
               stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"></svg>
        </button>
      </div>
      <button type="button" class="btn tiny" id="notif-btn" title="Browser reminders for due TODOs">
        <svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round">
          <path d="M4 6.2a4 4 0 0 1 8 0c0 3 1 4 1.4 4.6H2.6C3 10.2 4 9.2 4 6.2Z"/><path d="M6.6 13a1.5 1.5 0 0 0 2.8 0"/>
        </svg>
        <span id="notif-label">Reminders</span>
      </button>
      <select id="palette-select" title="Color theme">
        <optgroup label="Vivid">
          <option value="nebula">Nebula</option>
          <option value="synth">Synthwave</option>
          <option value="aurora">Aurora</option>
          <option value="sorbet">Sorbet</option>
        </optgroup>
        <optgroup label="Light">
          <option value="paper">Paper</option>
          <option value="nord">Nord</option>
          <option value="earth">Earth</option>
          <option value="twilight">Twilight</option>
        </optgroup>
        <optgroup label="Dark">
          <option value="midnight">Midnight</option>
          <option value="nord-dark">Nord Dark</option>
          <option value="earth-dark">Earth Dark</option>
          <option value="twilight-dark">Twilight Dark</option>
        </optgroup>
      </select>
    </div>
  </aside>

  <div id="mainwrap">
    <header id="topbar">
      <div class="navsearch">
        <svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6">
          <circle cx="7" cy="7" r="4.5"/><path d="M10.5 10.5L14 14"/>
        </svg>
        <input id="navsearch-input" placeholder="Search everything…">
        <span class="kbd">/</span>
      </div>
      <div class="stats" id="stats"></div>
      <button type="button" class="btn tiny" id="agent-btn">Brief a fresh agent</button>
    </header>

    <main>
      <div class="pane" id="list"></div>
    </main>
  </div>

</div>

<div id="toast"></div>

<dialog id="about">
  <img src="/icon-full.png" alt="">
  <div class="body">
    <h2>Loom</h2>
    <p>The weaver at the loom, working the same threads Loom keeps for you - jots pulled taut
       into memory, one strand at a time.</p>

    <h3>What this is</h3>
    <p>A single shared notebook. Every record - note, fact, task, reminder - is a <b>jot</b>:
       free text, plus optional tags and a summary. There's no separate "task" or "memory" type;
       a plain jot becomes a TODO just by picking up a <code>todo</code> tag, a priority, or a
       due date, and stops being one the moment none of those apply.</p>

    <h3>The four tabs</h3>
    <p><b>Dashboard</b> - the TODOs &amp; Reminders panel, recent activity, tag distribution,
       and drift warnings, all at a glance.<br>
       <b>Search</b> - find anything by text or tag; toggle card/list view; open a result to
       read or edit it.<br>
       <b>Tags</b> - every tag in use, with counts and possible near-duplicates flagged.<br>
       <b>Health</b> - store size, persistence (WAL/snapshot) status, and admin actions.</p>

    <h3>Naming and linking</h3>
    <p>Give a jot a <b>slug</b> (its optional <code>name</code>) to make it addressable: link to
       it from another jot with <code>[[slug]]</code>, or write to it again with the same slug to
       update it in place instead of creating a duplicate.</p>

    <h3>TODOs &amp; reminders</h3>
    <p>Open any jot and click <b>Make this a TODO</b> to reveal Priority and Due - or just add a
       <code>todo</code>/<code>warning</code>/<code>error</code> tag, which does the same thing
       automatically. TODOs get their own panel on the Dashboard, split into High/Normal/Low
       columns; drag a card between columns to reprioritize it. <b>Snooze</b> just pushes the due
       date later - there's no separate snoozed state to keep track of. <b>Complete</b> - the green
       checkbox at the bottom right of both the card and the dialog - adds a
       <code>status:done</code> tag and drops it out of the panel, keeping <code>todo</code> so the
       finished work stays on the record; hit the same button again to bring it back exactly as
       it was.</p>

    <h3>Notifications</h3>
    <p>The <b>Reminders</b> button in the header asks the browser for permission to show
       notifications. Once granted, this tab checks for due TODOs about once a minute and fires
       one notification 15 minutes before something's due and another right at the time - only
       while this tab is open. If the button says "blocked," your browser was told no at some
       point; undo that from the browser's own site-permissions UI (the icon in the address bar
       is the usual way in), then reload.</p>

    <h3>Shortcuts</h3>
    <p><code>/</code> jumps to the search box from anywhere. <code>Esc</code> closes whatever's
       open. <code>Ctrl/Cmd+Enter</code> saves the open jot.</p>

    <button type="button" class="btn" id="about-close">Close</button>
  </div>
</dialog>

<dialog id="agent-dialog">
  <div class="body">
    <h2>Brief a fresh agent</h2>
    <p>Paste this at the start of a new session so it knows Loom is here before it assumes
       anything about this project.</p>
    <div class="promptbox open" id="agent-prompt-text"></div>
    <div class="row" style="margin-top:12px">
      <button type="button" class="btn primary" id="agent-copy">Copy prompt</button>
      <button type="button" class="btn ghost" id="agent-close">Close</button>
    </div>
  </div>
</dialog>

<dialog id="purge-dialog">
  <div class="body">
    <div id="purge-ask">
      <h2>Request a purge</h2>
      <p>This erases <b id="purge-count"></b> from the snapshot, the write-ahead log and the history
         log - every version, not just the current one. It is the tool for something that should
         never have been written down, and it cannot be undone by anything, because the undo log is
         one of the things being erased.</p>
      <p>Nothing is erased now. This writes a request file and hands you a short procedure to
         follow - or to give an agent - which stops the service, purges, and starts it again.</p>
      <label for="purge-reason">Why</label>
      <input type="text" id="purge-reason" placeholder="e.g. pasted a live API key into a note">
      <div class="row" style="margin-top:14px">
        <button type="button" class="btn badfill" id="purge-submit">Write the request</button>
        <button type="button" class="btn ghost" id="purge-cancel">Cancel</button>
      </div>
    </div>
    <div id="purge-done" style="display:none">
      <h2>Purge requested</h2>
      <p>Nothing has been erased yet. Follow these steps, or paste them to an agent - the
         confirmation in step 1 is the point, so do not skip it.</p>
      <div class="purgesteps" id="purge-step"></div>
      <div class="row">
        <button type="button" class="btn primary" id="purge-copy">Copy instructions</button>
        <button type="button" class="btn ghost" id="purge-close">Close</button>
      </div>
    </div>
  </div>
</dialog>

<dialog id="acl-dialog">
  <div class="body">
    <h2>Access list</h2>
    <p>When this is on, Loom answers only the addresses listed here and refuses everything else -
       the dashboard, the API and MCP alike.</p>
    <div id="acl-err"></div>
    <div class="aclswitch">
      <input type="checkbox" id="acl-enabled">
      <label for="acl-enabled">Only answer listed addresses</label>
      <span class="sub" id="acl-count"></span>
    </div>
    <div class="aclcaller">
      You are <b id="acl-caller"></b>
      <button type="button" class="btn tiny" id="acl-addme">Add this address</button>
    </div>
    <div class="aclrules" id="acl-rules"></div>
    <div class="row" style="margin-bottom:14px">
      <input type="text" id="acl-input" placeholder="192.168.1.0/24  or  10.0.0.7"
             style="flex:2;min-width:120px">
      <input type="text" id="acl-note" placeholder="note (optional)" style="flex:1;min-width:90px">
      <button type="button" class="btn" id="acl-add">Add</button>
    </div>
    <p class="aclnote">This machine can always reach Loom, whatever the list says - that is what
       stops a typo here from locking you out permanently. Repair a bad list from a browser or
       curl on the server itself.</p>
    <div class="row">
      <button type="button" class="btn primary" id="acl-save">Save</button>
      <button type="button" class="btn ghost" id="acl-cancel">Cancel</button>
    </div>
  </div>
</dialog>

<dialog id="detail-dialog">
  <button type="button" class="dclose" id="detail-close-x" aria-label="Close">
    <svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6">
      <path d="M3 3l10 10M13 3L3 13"/>
    </svg>
  </button>
  <div class="pane" id="detail"></div>
</dialog>

<script>
const $=s=>document.querySelector(s);
const el=(t,c,x)=>{const e=document.createElement(t);if(c)e.className=c;
                   if(x!==undefined)e.textContent=x;return e;};
const escHtml=s=>(s??'').toString().replace(/[&<>"]/g,c=>
  ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));

/* sortOrder/sinceWhen live here, not inside viewSearch(), for the same reason activeTags and
   lastQ do: viewSearch() is torn down and rebuilt on every render() - including the render() that
   just opening a result triggers - so filter state kept local to it would silently reset the
   instant a jot is clicked. */
let view='dashboard',sel=null,activeTags=new Set(),allTags=[],lastQ='',stats={},
    sortOrder='',sinceWhen='';
let cardMode='cards';try{cardMode=localStorage.getItem('loom-cardmode')||'cards';}catch(e){}
/* The detail dialog defaults to a minimal summary/priority/due view; detailExpanded tracks whether
   the current jot has been switched to the full form. Keyed by detailOpenedKey so it resets to
   minimal the moment a DIFFERENT jot is opened, rather than leaking "expanded" across selections. */
let detailExpanded=false,detailOpenedKey=null,detailForceTodo=false;

/* undoFn turns a toast into a brief "in case that was a mistake" window - the checkmark on a
   TODO row, or a drag between priority columns, are both one accidental click/slip away from
   changing something, so the actions most likely to be hit by mistake carry an inline Undo
   rather than requiring a confirm dialog on every click (which would make the common, intended
   case slower to punish the rare accidental one). Stays up longer than a plain toast so there's
   real time to catch it. */
function toast(msg,kind,undoFn){
  const t=$('#toast');t.innerHTML='';t.append(document.createTextNode(msg));
  if(undoFn){
    const b=el('button','undo','Undo');b.type='button';
    b.onclick=function(){clearTimeout(t._t);t.className='';undoFn();};
    t.append(b);
  }
  t.className='show '+(kind||'ok');
  clearTimeout(t._t);t._t=setTimeout(()=>t.className='',undoFn?6000:3200);
}

async function api(path,opts){
  const r=await fetch(path,opts);const text=await r.text();
  let body=null;try{body=text?JSON.parse(text):null;}catch(e){body={error:text};}
  if(!r.ok)throw Object.assign(new Error(body&&body.error?body.error:r.statusText),
                               {status:r.status,body});
  return body;
}

/* Ids are UTC microseconds; the browser renders them in the reader's own zone, which is the zone
   these notes were written in. */
const D=us=>new Date(us/1000);
function ago(us){
  const s=(Date.now()-us/1000)/1000;
  if(s<45)return'just now';
  if(s<3600)return Math.round(s/60)+'m ago';
  if(s<86400)return Math.round(s/3600)+'h ago';
  if(s<86400*7)return Math.round(s/86400)+'d ago';
  const d=D(us);
  return d.toLocaleDateString(undefined,{month:'short',day:'numeric',
    year:d.getFullYear()===new Date().getFullYear()?undefined:'numeric'});
}
const dayKey=us=>D(us).toDateString();
function dayLabel(us){
  const d=D(us),t=new Date();
  if(d.toDateString()===t.toDateString())return'Today';
  const y=new Date(t.getTime()-864e5);
  if(d.toDateString()===y.toDateString())return'Yesterday';
  return d.toLocaleDateString(undefined,{weekday:'long',month:'long',day:'numeric',
    year:d.getFullYear()===t.getFullYear()?undefined:'numeric'});
}
const stamp=us=>D(us).toLocaleString(undefined,
  {year:'numeric',month:'short',day:'numeric',hour:'2-digit',minute:'2-digit'});

/* ---------- header ---------- */
/* Counters go to the topbar readout; connection state goes to the floor of the rail. They used to
   sit in the same header cluster, which put "RAM only" - a thing you need to notice - at the same
   visual weight as a jot count you glance at. */
function setLive(cls,text){
  const H=$('#live-slot');H.innerHTML='';
  const L=el('div','live');L.append(el('i','dot'+cls));L.append(el('span',null,text));
  H.append(L);
}
async function refreshStats(){
  const S=$('#stats');
  try{
    stats=await api('/stats');const p=stats.persistence||{};
    S.innerHTML='';
    const add=(v,k)=>{const w=el('div','stat');w.append(el('b',null,v));
                      w.append(el('span',null,k));S.append(w);};
    add(stats.jots,'jots');add(stats.named,'named');add(stats.tags,'tags');
    if(p.enabled)add((p.wal_bytes/1024).toFixed(0)+'k','wal');
    setLive(p.enabled?'':' off',p.enabled?'persistent':'RAM only');
    drawNav();
  }catch(e){
    S.innerHTML='';
    setLive(' off','unreachable');
  }
}

const NAV_ICONS={
  dashboard:'<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linejoin="round">'+
    '<rect x="1.8" y="1.8" width="5" height="5" rx="1.2"/><rect x="9.2" y="1.8" width="5" height="5" rx="1.2"/>'+
    '<rect x="1.8" y="9.2" width="5" height="5" rx="1.2"/><rect x="9.2" y="9.2" width="5" height="5" rx="1.2"/></svg>',
  search:'<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round">'+
    '<circle cx="7" cy="7" r="4.5"/><path d="M10.5 10.5L14 14"/></svg>',
  tags:'<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round">'+
    '<path d="M6.2 2 4.7 14M11.3 2 9.8 14M3 6h11M2 10h11"/></svg>',
  health:'<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">'+
    '<path d="M1.8 8h3l1.6-4 2.6 8 1.7-4h3.5"/></svg>',
  history:'<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">'+
    '<path d="M2.4 8a5.6 5.6 0 1 0 1.7-4"/><path d="M2 2.6V6h3.4"/><path d="M8 5.2V8l2 1.4"/></svg>'
};
const VIEWS=[['dashboard','Dashboard'],['search','Search'],['tags','Tags'],
             ['history','History'],['health','Health']];
let navTabEls=[];
function drawNav(){
  const N=$('#nav');N.innerHTML='';
  navTabEls=[];
  VIEWS.forEach(function(v){
    const b=el('button',v[0]===view?'on':'');
    const ic=el('span','nvi');ic.innerHTML=NAV_ICONS[v[0]]||'';b.append(ic);
    b.append(document.createTextNode(v[1]));
    if(v[0]==='tags'&&stats.tags!==undefined)b.append(el('span','pill',stats.tags));
    if(v[0]==='dashboard'&&stats.jots!==undefined)b.append(el('span','pill',stats.jots));
    b.onclick=function(){view=v[0];drawNav();render();};
    b.dataset.view=v[0];
    navTabEls.push(b);
    N.append(b);
  });
  /* The topbar input is no longer rebuilt here, so it has to be re-synced instead - but never
     while it has focus, or a periodic refreshStats() would yank the caret mid-word. */
  const ni=$('#navsearch-input');
  if(ni&&document.activeElement!==ni&&ni.value!==lastQ)ni.value=lastQ;
}

/* Search is static markup in the topbar, wired exactly once - it is reachable from every view, and
   typing in it switches to Search and runs the query. It deliberately does NOT live inside
   drawNav() any more: drawNav() is called from refreshStats() on a timer, and rebuilding the input
   out from under a typing user destroyed the element (and their cursor position) mid-keystroke. */
(function(){
  const ni=$('#navsearch-input');
  let nt;
  ni.oninput=function(){
    clearTimeout(nt);
    nt=setTimeout(function(){
      lastQ=ni.value;
      view='search';
      navTabEls.forEach(b=>b.classList.toggle('on',b.dataset.view==='search'));
      render();
    },160);
  };
})();

/* ---------- cards ---------- */
function highlight(text,terms){
  const safe=escHtml(text);
  if(!terms.length)return safe;
  const esc=terms.map(t=>t.replace(/[.*+?^${}()|[\]\\]/g,'\\$&'));
  return safe.replace(new RegExp('('+esc.join('|')+')','gi'),'<mark>$1</mark>');
}

/* Category = the first non-structural tag (":"-tags are structure, not vocabulary - same rule
   the server's drift/vocabulary logic uses). No dedicated field for it; deterministically hashed
   onto a small fixed set of colors already in the palette so the same tag always reads the same
   way, without inventing a new one-color-per-tag system. */
const CAT_VARS=['--accent','--warn','--good','--dim'];
/* Tags meaning "you may need to act on this", independent of topic - see the .tag.action rule. */
const ACTION_TAGS=new Set(['todo','warning','warn','error','bug']);
const tagClass=t=>'tag'+(t.indexOf(':')>=0?' res':'')+(ACTION_TAGS.has(t)?' action':'');

/* TODO/reminder metadata rides structural tags - due:<local-datetime>, priority:high|normal|low,
   done - rather than new wire fields. Colon tags are already excluded from the free vocabulary
   (see catColorOf/TagRegistry::IsReserved), so this is the same rule applied to one more concern
   instead of a second field system bolted onto one jot "type" that isn't really a separate type
   at all. Deliberately ONE date, not a due date plus a separate snooze state to track: "snooze"
   is just an action that reschedules `due` to later - nothing needs to remember that a jot was
   ever snoozed, because there's nothing left to remember once the due date itself has moved. */
/* Tags are lowercased server-side (vocabulary normalization), which turns the ISO "T" separator
   into "t" - fine for Date parsing (accepted either way) but not for a datetime-local input's
   value, which is picky about exact case. Normalize back to "T" the moment a due value comes off
   a tag, so round-tripping through the picker doesn't silently blank it out. */
const tagValue=(tags,prefix)=>{const t=(tags||[]).find(x=>x.indexOf(prefix)===0);
  return t?t.slice(prefix.length).replace('t','T'):null;};
const toLocalDT=v=>v?new Date(v.length>10?v:v+'T00:00'):null;
const dueOf=j=>toLocalDT(tagValue(j.tags,'due:'));
function priorityOf(j){
  const p=tagValue(j.tags,'priority:');
  if(p==='high'||p==='normal'||p==='low')return p;
  return((j.tags||[]).some(t=>ACTION_TAGS.has(t))||dueOf(j))?'normal':null;
}
/* Whether a jot gets the TODO treatment at all (priority/due fields, the panel, notifications) -
   gated so opening an ordinary jot never shows task controls it has no use for. A completed jot
   still counts, so its dialog keeps showing priority/due plus the reopen button. */
function isTodo(j){
  return isDone(j)||(j.tags||[]).some(t=>ACTION_TAGS.has(t))||
    !!tagValue(j.tags,'due:')||!!tagValue(j.tags,'priority:');
}
function dueLabel(d){
  const ms=d.getTime()-Date.now(),hr=3600000,day=86400000,abs=Math.abs(ms),overdue=ms<0;
  let mag;
  if(abs<hr)mag=Math.max(1,Math.round(abs/60000))+'m';
  else if(abs<day)mag=Math.round(abs/hr)+'h';
  else{const n=Math.round(abs/day);mag=n+(n===1?' day':' days');}
  return(overdue?'Overdue by ':'Due in ')+mag;
}
function toLocalInputValue(d){
  const pad=n=>String(n).padStart(2,'0');
  return d.getFullYear()+'-'+pad(d.getMonth()+1)+'-'+pad(d.getDate())+'T'+pad(d.getHours())+':'+pad(d.getMinutes());
}
async function setPriority(j,newP){
  const tags=(j.tags||[]).filter(t=>t.indexOf('priority:')!==0);
  tags.push('priority:'+newP);
  const exp=j.updated||j.id;
  return api('/jots/'+j.id+'?expect_updated='+exp,
    {method:'PATCH',headers:{'Content-Type':'application/json'},body:JSON.stringify({tags})});
}
async function setDue(j,val){
  const tags=(j.tags||[]).filter(t=>t.indexOf('due:')!==0);
  if(val)tags.push('due:'+val);
  const exp=j.updated||j.id;
  return api('/jots/'+j.id+'?expect_updated='+exp,
    {method:'PATCH',headers:{'Content-Type':'application/json'},body:JSON.stringify({tags})});
}
/* Completed ADDS `status:done` alongside `todo` - it does not replace `todo`/priority/due. Keeping
   both is what makes the todo tag a history of work rather than a list that forgets: open work is
   `todo` minus `status:done`. It's also what makes "completed can go back to being a TODO" free -
   drop the one tag and nothing else about the jot has changed. */
const isDone=j=>(j.tags||[]).includes('status:done');
async function toggleDone(j,markDone){
  const tags=(j.tags||[]).filter(t=>t!=='status:done');
  if(markDone)tags.push('status:done');
  const exp=j.updated||j.id;
  return api('/jots/'+j.id+'?expect_updated='+exp,
    {method:'PATCH',headers:{'Content-Type':'application/json'},body:JSON.stringify({tags})});
}
/* The check-in-a-box, defined once. Both Complete buttons draw from here so they can't drift
   apart the way two hand-written copies of an icon do. */
const CHECKBOX_SVG='<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.7" '+
  'stroke-linecap="round" stroke-linejoin="round"><rect x="2.3" y="2.3" width="11.4" '+
  'height="11.4" rx="2.6"/><path d="M5 8.2l2.1 2.1L11.2 6"/></svg>';
function completeBtn(bDone,fn){
  const b=el('button','completebtn'+(bDone?' on':''));
  b.type='button';b.draggable=false;
  b.innerHTML=CHECKBOX_SVG;
  b.append(document.createTextNode(bDone?'Completed':'Complete'));
  b.title=bDone?'Reopen this TODO':'Mark this TODO completed';
  b.onclick=function(e){e.stopPropagation();e.preventDefault();fn();};
  return b;
}
function hashCat(name){
  let h=0;for(let i=0;i<name.length;i++)h=(h*31+name.charCodeAt(i))>>>0;
  return{cssVar:CAT_VARS[h%CAT_VARS.length],name:name};
}
function catColorOf(tags){
  /* An action tag is never the topic. "todo" says what to DO with a jot, not what it's ABOUT, and
     letting it win here made every open task claim `todo` as its category - which put a bogus
     "todo" bar at the top of the distribution chart, painted unrelated tasks the same category
     color, and left the real topic off the TODO card entirely. Prefer the first genuine topic tag;
     only fall back to an action tag when a jot truly has nothing else. */
  const bare=(tags||[]).filter(x=>x.indexOf(':')<0);
  const t=bare.find(x=>!ACTION_TAGS.has(x)&&x!=='done')||bare[0];
  if(t)return hashCat(t);
  // No free-form tag - fall back to type:<x> rather than claiming "untagged" when
  // structural tags actually exist (a jot can be all-reserved-tags and still be tagged).
  const typeTag=(tags||[]).find(x=>x.indexOf('type:')===0);
  if(typeTag)return hashCat(typeTag.slice(5));
  return{cssVar:'--dim',name:(tags||[]).length?'untyped':'untagged'};
}

function jotCard(j,maxScore,terms){
  const isMem=!!j.name;
  const cat=catColorOf(j.tags);
  const done=isDone(j);
  const c=el('div','mcard'+(sel&&sel.id===j.id?' sel':'')+(done?' done':''));
  c.style.setProperty('--cat','var('+cat.cssVar+')');

  /* Reserved tags (type:x, status:x) and the editor stay out of the card face entirely - they're
     already implied by the pill/detail and just added noise repeated on every card. A card is a
     thing to recognize and click, not the full record. */
  const head=el('div','cathead');
  head.append(el('span','catpill',cat.name));
  c.append(head);

  /* The slug is the fast-recognition handle for a named jot - short, stable, chosen on purpose - so
     it gets the big type. Everything else (summary, or text when there's no summary) is one quiet
     line underneath, not competing for the same attention. A jot with no name has no slug to lead
     with, so its own text/summary steps up into the big spot instead. */
  if(isMem){
    c.append(el('div','title',j.name));
    const sub=j.summary||j.text;
    if(sub){const s=el('div','headline');s.innerHTML=highlight(sub,terms);c.append(s);}
  }else if(j.summary){
    const s=el('div','title');s.innerHTML=highlight(j.summary,terms);c.append(s);
    if(j.text){const t=el('div','headline');t.innerHTML=highlight(j.text,terms);c.append(t);}
  }else{
    const t=el('div','title');t.innerHTML=highlight(j.text||'',terms);c.append(t);
  }

  const f=el('div','mfoot');
  /* Priority/due only surface when they say something a normal card doesn't already imply - a
     `normal` priority chip on every third card would just be more noise to filter past. */
  const prio=priorityOf(j),due=dueOf(j);
  if(done){
    const tb=el('span','taskbits');tb.append(el('span','donechip','done'));f.append(tb);
  }else if((prio&&prio!=='normal')||due){
    const tb=el('span','taskbits');
    if(prio&&prio!=='normal')tb.append(el('span','priochip pr-'+prio,prio));
    if(due){
      const overdue=due.getTime()<Date.now();
      tb.append(el('span','duechip'+(overdue?' over':''),dueLabel(due)));
    }
    f.append(tb);
  }
  /* Action tags sort first so a `todo`/`warning`/`error` tag is never lost to the 2-tag clip -
     it's the whole point of flagging it. */
  const folks=(j.tags||[]).filter(t=>t.indexOf(':')<0&&t!=='done');
  const shown=folks.slice().sort((a,b)=>(ACTION_TAGS.has(b)?1:0)-(ACTION_TAGS.has(a)?1:0));
  shown.slice(0,2).forEach(x=>f.append(el('span',tagClass(x),'#'+x)));
  const trail=el('span','mtrail');
  if(j.score!==undefined)trail.append(el('span','scbadge',j.score.toFixed(1)));
  const when=el('span','when',ago(j.id));when.title=stamp(j.id);trail.append(when);
  f.append(trail);
  c.append(f);

  c.onclick=function(){sel=j;render();};
  return c;
}

const queryTerms=q=>(q||'').toLowerCase().split(/[^a-z0-9']+/i).filter(t=>t.length>1);

/* ---------- search ----------
   Filters (sort, time range, tags) all live above the results, and combine - this is the "search
   is a first-class citizen" pass: previously it was a text box plus a tag-chip row with no time
   filter. Sort/tag/time all map straight onto Query.h's existing order/tag/since params. */
async function viewSearch(target){
  const L=target;

  /* No search box of its own any more. The topbar's field is always on screen and already drives
     this view, so a second input sitting directly under it was two places to type the same query,
     with only one of them holding the value after a tab switch. This view reads that field. */
  const q=$('#navsearch-input');

  const bar=el('div','filterbar');

  const sortG=el('div','fgroup');sortG.append(el('span','flabel','Sort'));
  const order=el('select');order.style.width='auto';
  [['','Best match'],['newest','Newest first'],['oldest','Oldest first']]
    .forEach(function(o){const x=el('option',null,o[1]);x.value=o[0];order.append(x);});
  order.value=sortOrder;
  sortG.append(order);bar.append(sortG);

  const whenG=el('div','fgroup');whenG.append(el('span','flabel','When'));
  const when=el('select');when.style.width='auto';
  [['','All time'],['1d','Today'],['7d','This week'],['30d','This month']]
    .forEach(function(o){const x=el('option',null,o[1]);x.value=o[0];when.append(x);});
  when.value=sinceWhen;
  whenG.append(when);bar.append(whenG);

  const vt=el('div','viewtoggle');
  const cardsBtn=el('button');cardsBtn.type='button';cardsBtn.title='Card view';
  cardsBtn.innerHTML='<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4">'+
    '<rect x="1.5" y="1.5" width="6" height="6" rx="1"/><rect x="8.5" y="1.5" width="6" height="6" rx="1"/>'+
    '<rect x="1.5" y="8.5" width="6" height="6" rx="1"/><rect x="8.5" y="8.5" width="6" height="6" rx="1"/></svg>';
  const listBtn=el('button');listBtn.type='button';listBtn.title='List view';
  listBtn.innerHTML='<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4">'+
    '<path d="M1.5 3h13M1.5 8h13M1.5 13h13"/></svg>';
  const setMode=function(m){cardMode=m;try{localStorage.setItem('loom-cardmode',m);}catch(e){}
    cardsBtn.classList.toggle('on',m==='cards');listBtn.classList.toggle('on',m==='list');
    grid.classList.toggle('list',m==='list');};
  cardsBtn.onclick=()=>setMode('cards');listBtn.onclick=()=>setMode('list');
  cardsBtn.classList.toggle('on',cardMode==='cards');listBtn.classList.toggle('on',cardMode==='list');
  vt.append(cardsBtn,listBtn);bar.append(vt);

  /* No "New jot" button here either - the rail carries both create actions on every view now, so
     a third copy in this one filter bar is just another thing to keep in sync. */
  L.append(bar);

  /* The chip row is rebuilt from state on every toggle rather than painted once. Two reasons, both
     of which stranded a filter with no way off: a chip that flipped the filter but kept its old
     on/off look gave no feedback at all, and - worse - the row used to be *only* the top slice of
     the /tags vocabulary, which never returns reserved tags. Every other path that sets activeTags
     (a Top Tags pill, the TODO card, a tag-drift row) could therefore select a tag with no chip
     anywhere on screen, and the only escape was a reload. Actives are now rendered from activeTags
     itself, so whatever is filtering is always visible and always clickable. */
  const chips=el('div','chips');L.append(chips);
  function chipFor(tag,count,on){
    const c=el('span','chip'+(on?' on':''));
    c.append(document.createTextNode(tag));
    if(count!==undefined)c.append(el('b',null,count));
    if(on)c.append(el('i','x','×'));
    c.title=(on?'Remove filter: ':'Filter by ')+tag;
    c.onclick=function(){on?activeTags.delete(tag):activeTags.add(tag);drawChips();run();};
    return c;
  }
  function drawChips(){
    chips.innerHTML='';
    const counts={};allTags.forEach(function(t){counts[t.tag]=t.count;});
    activeTags.forEach(t=>chips.append(chipFor(t,counts[t],true)));
    let room=18-activeTags.size;
    for(let i=0;i<allTags.length&&room>0;i++){
      if(activeTags.has(allTags[i].tag))continue;
      chips.append(chipFor(allTags[i].tag,allTags[i].count,false));room--;
    }
    if(activeTags.size){
      const clr=el('span','chip clearall','Clear filters');
      clr.title='Remove all tag filters';
      clr.onclick=function(){activeTags=new Set();drawChips();run();};
      chips.append(clr);
    }
    chips.style.display=chips.childElementCount?'':'none';
  }
  drawChips();

  const meta=el('div','rmeta');L.append(meta);
  const grid=el('div','cardgrid'+(cardMode==='list'?' list':''));L.append(grid);

  async function run(){
    lastQ=q.value;
    const p=new URLSearchParams();
    if(q.value){p.set('q',q.value);p.set('prefix','1');}
    activeTags.forEach(t=>p.append('tag',t));
    if(order.value)p.set('order',order.value);
    if(when.value)p.set('since',when.value);
    p.set('limit','60');
    try{
      const r=await api('/jots?'+p);
      const jots=r.jots;

      meta.innerHTML='';
      meta.append(el('em',null,jots.length+(jots.length===1?' match':' matches')));
      if(r.truncated)meta.append(el('span',null,'showing '+r.returned));
      if(activeTags.size)meta.append(el('span',null,'tag-filtered'));

      grid.innerHTML='';
      if(!jots.length){
        const e=el('div','empty');
        e.append(el('b',null,'Nothing matched'));
        e.append(el('div',null,q.value?'Try fewer words — summaries are weighted highest.'
                                      :'Clear the filters, or write something.'));
        grid.append(e);return;
      }
      let max=0;jots.forEach(j=>{if((j.score||0)>max)max=j.score||0;});
      const terms=queryTerms(q.value);
      jots.forEach(j=>grid.append(jotCard(j,max,terms)));
    }catch(e){grid.innerHTML='';grid.append(el('div','note bad',e.message));}
  }

  /* q's own oninput is owned by the topbar wiring (it re-renders this whole view on every
     debounced keystroke), so this view must not overwrite it - it only listens to the controls
     it actually owns. */
  order.onchange=function(){sortOrder=order.value;run();};
  when.onchange=function(){sinceWhen=when.value;run();};
  await run();
  /* Arriving at Search with nothing else claiming the caret should put it in the search field -
     but never yank it away from something the user is already typing in (including the field
     itself, mid-keystroke, which is what re-rendered this view in the first place). */
  if(!sel&&(document.activeElement===document.body||document.activeElement===null))q.focus();
}

/* ---------- dashboard: at-a-glance home view ---------- */
/* The brief a fresh agent starts from. Built live so the origin and the corpus size in it are
   the real ones - a prompt with a stale port or an invented jot count is worse than none. */
function agentPrompt(){
  const o=location.origin;
  const n=(stats&&stats.jots!==undefined)?stats.jots:'?';
  const m=(stats&&stats.named!==undefined)?stats.named:'?';
  return [
"INIT ONLY - DO EXACTLY THIS, THEN STOP:",
"0. If Loom's MCP tools (loom_search, loom_get, ...) are NOT already available in this session,",
"   register the server, then tell Alex to start a NEW session before going any further - MCP",
"   servers are only picked up at session start, never mid-session:",
"     Claude Code:  claude mcp add --transport http loom "+o+"/mcp",
"     Codex CLI:    codex mcp add loom --url "+o+"/mcp",
"     Other agents: add a remote/Streamable HTTP MCP server pointing at "+o+"/mcp, or skip this",
"                   and use the REST calls below instead.",
"1. One call to skim what's here: loom_search(order=newest, limit=20, brief=true) over MCP if",
"   connected, else GET "+o+"/jots?order=newest&limit=20&brief=1. brief drops jot bodies so the",
"   skim stays cheap - only loom_get a hit if a summary alone isn't enough.",
"2. Report back briefly, then wait for an actual request. Do not chain into builds, tests,",
"   deeper reads, or any other work just because this prompt loaded - nothing below this line",
"   is a task.",
"",
"This project's shared memory is Loom ("+o+"), holding "+n+" jots ("+m+" named),",
"replacing the old H:\\\\Alex\\\\dev\\\\.claude markdown store as source of truth while reachable.",
"",
"How Alex wants answers: concise, outline form - a top-level bullet per point, detail nested",
"under it. He'll dive in and ask for more if he wants it; skip lengthy rationale unless it's",
"important to the work at hand or worth remembering for later. Add a `todo` jot for anything",
"noticed that should be addressed later instead of doing it now.",
"",
"--- reference below, not needed for init ---",
"",
"REST, no registration needed:",
"  GET  "+o+"/jots?q=<terms>          search, best match first",
"  GET  "+o+"/jots?order=newest&limit=20   what changed lately",
"  GET  "+o+"/jots/by-name/<slug>     one named jot",
"  POST "+o+"/jots                    {name,summary,text,tags,links,editor}",
"  &brief=1 on any /jots search       drops jot bodies - id/name/summary/tags only, for a skim",
"",
"How Loom works:",
"- Every fact is a jot. Giving one a `name` (slug) makes it addressable by [[link]] and lets a",
"  later write upsert it instead of creating a duplicate - it isn't a separate class of record.",
"  Prefer updating an existing named jot over writing a second one about the same thing.",
"- `summary` is the one-line fact and is weighted 3x in ranking; `text` is the supporting",
"  detail and is optional. A terse jot can be summary-only.",
"- Tags with a colon are structural (type:project, asserted:2026-08-31, status:superseded).",
"  Bare tags are topical. Both are searchable.",
"- Writes answer with a `warnings` array when a tag nearly duplicates an existing one. The",
"  write still succeeded - fix the tag and write again rather than ignoring it.",
"- Link related jots by slug in `links`. A slug that does not exist yet is kept pending",
"  and connects itself the moment something takes that name.",
"",
"When you learn something durable in this session, write it back before the session ends."
  ].join("\n");
}

function copyText(t,btn){
  const done=function(){const was=btn.textContent;btn.textContent='Copied';
    setTimeout(function(){btn.textContent=was;},1400);};
  if(navigator.clipboard&&navigator.clipboard.writeText){
    navigator.clipboard.writeText(t).then(done,function(){toast('Copy blocked by the browser','err');});
    return;
  }
  /* file:// and non-secure origins have no async clipboard - the old path still works there */
  const ta=el('textarea');ta.value=t;ta.style.cssText='position:fixed;opacity:0';
  document.body.append(ta);ta.select();
  try{document.execCommand('copy');done();}catch(e){toast('Copy blocked by the browser','err');}
  ta.remove();
}

const OV_ICONS={
  dot:'<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6">'+
      '<circle cx="8" cy="8" r="3.2"/></svg>',
  layers:'<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linejoin="round">'+
      '<path d="M8 2 2 5.3 8 8.6l6-3.3L8 2Z"/><path d="M2 8.7 8 12l6-3.3M2 11.7 8 15l6-3.3"/></svg>',
  hash:'<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round">'+
      '<path d="M6.2 2 4.7 14M11.3 2 9.8 14M3 6h11M2 10h11"/></svg>',
  flag:'<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round">'+
      '<path d="M3 14V2"/><path d="M3 3h9l-2.3 3L12 9H3"/></svg>'
};

/* ---------- dashboard: overview ----------
   One bulk brief=1 fetch (see loom-todo-summary-only-listing) drives both the distribution bars
   and the activity list - a topic-level skim has no business pulling every jot body over the
   wire twice. */
async function viewDashboard(target){
  const L=target;
  try{
    const [tags,sim,recent,health]=await Promise.all([
      api('/tags'), api('/tags/similar'),
      api('/jots?order=newest&limit=200&brief=1'), api('/stats')
    ]);
    const topTags=tags.tags.slice().sort((a,b)=>b.count-a.count);
    const p=health.persistence||{};

    /* ---- todos & reminders ----
       Sits above the stat cards on purpose - open work is the thing to act on next, everything
       else below is context for deciding what to do about it. Pulled from the same brief=1 fetch
       as activity/distribution, so it inherits the same newest-200 cap rather than a second call. */
    const todos=recent.jots.filter(j=>!isDone(j)&&isTodo(j));
    const todoP=el('div','ov-panel ov-todo');L.append(todoP);
    const th=el('div','phead');
    const badge=el('div','ov-todobadge');badge.innerHTML=OV_ICONS.flag;th.append(badge);
    const th1=el('div','pheadmain');
    th1.append(el('div','eyebrow','TODOS & REMINDERS'));
    th1.append(el('h3',null,todos.length?todos.length+' open':'Nothing outstanding'));
    th.append(th1);
    th.append(el('a',null,'Open in Search'));
    th.lastChild.onclick=function(){view='search';activeTags=new Set(['todo']);drawNav();render();};
    todoP.append(th);
    if(!todos.length){
      todoP.append(el('div','empty','Nothing tagged todo, warning, or error, and nothing due. Clear.'));
    }else{
      /* Split by priority rather than one flat list - high-priority work should never be scrolled
         past to find it. Due date breaks ties within a column, soonest (or most overdue) first;
         undated items sink to the bottom since there's nothing urgent to say about them yet. */
      const cols=el('div','ov-todocols');todoP.append(cols);
      const byPrio={high:[],normal:[],low:[]};
      todos.forEach(j=>byPrio[priorityOf(j)||'normal'].push(j));
      ['high','normal','low'].forEach(function(p){
        const list=byPrio[p].slice().sort(function(a,b){
          const da=dueOf(a),db=dueOf(b);
          if(da&&db)return da-db;
          if(da)return-1;
          if(db)return 1;
          return b.id-a.id;
        });
        const col=el('div','ov-todocol pr-'+p);cols.append(col);
        const ch=el('div','ov-todocolhead');
        ch.append(el('span','ptitle',p));
        ch.append(el('span','pcount',String(list.length)));
        col.append(ch);
        const body=el('div','ov-todobody');col.append(body);
        /* Drag a card from one column to another to reprioritize it - a click-through to the full
           editor just to flip one select box is friction the panel doesn't need. Reordering within
           a column stays sort-driven (by due date) rather than draggable, on purpose. */
        col.addEventListener('dragover',function(e){e.preventDefault();col.classList.add('dragover');});
        col.addEventListener('dragleave',function(){col.classList.remove('dragover');});
        col.addEventListener('drop',async function(e){
          e.preventDefault();col.classList.remove('dragover');
          const id=e.dataTransfer.getData('text/plain');
          const j=todos.find(x=>String(x.id)===id);
          if(!j||(priorityOf(j)||'normal')===p)return;
          const prevP=priorityOf(j)||'normal';
          try{
            const updated=await setPriority(j,p);
            toast('Moved to '+p,'ok',async function(){
              try{await setPriority(updated,prevP);toast('Moved back to '+prevP);render();}
              catch(err){toast(err.message,'err');}
            });
            render();
          }catch(e){toast(e.message,'err');}
        });
        if(!list.length){body.append(el('div','ov-colempty','Nothing here.'));return;}
        list.slice(0,6).forEach(function(j){
          const due=dueOf(j);
          const cat=catColorOf(j.tags);
          const r=el('div','ov-trow');
          r.style.setProperty('--cat','var('+cat.cssVar+')');
          r.draggable=true;
          r.addEventListener('dragstart',function(e){
            e.dataTransfer.setData('text/plain',String(j.id));
            e.dataTransfer.effectAllowed='move';
            r.classList.add('dragging');
          });
          r.addEventListener('dragend',function(){r.classList.remove('dragging');});

          /* 1. chips: the action tag it carries, its priority, and its topic */
          const chips=el('div','ov-tchips');
          const act=(j.tags||[]).find(t=>ACTION_TAGS.has(t));
          chips.append(el('span','ov-tchip todo',act||'todo'));
          chips.append(el('span','ov-tchip pr-'+p,p));
          if(cat.name&&cat.name!==act)chips.append(el('span','ov-tchip scope',cat.name));
          chips.append(el('span','ov-tid',j.editor||'user'));
          r.append(chips);

          /* 2. what it is - the slug when it has one, else the first line of what it says */
          r.append(el('div','ov-atitle',
            j.name||(j.summary||j.text||'').slice(0,90)||'(untitled)'));

          /* 3. when - absolute stamp, with the relative magnitude trailing it */
          if(due){
            const overdue=due.getTime()<Date.now();
            const dl=el('div','ov-tdue'+(overdue?' over':''));
            dl.innerHTML='<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" '+
              'stroke-width="1.6" stroke-linecap="round"><circle cx="8" cy="8" r="6.2"/>'+
              '<path d="M8 4.6V8l2.4 1.6"/></svg>';
            dl.append(document.createTextNode((overdue?'Overdue · ':'Due · ')+stamp(due.getTime()*1000)));
            dl.append(el('span','rel','('+dueLabel(due).replace(/^(Overdue by|Due in) /,'')+')'));
            r.append(dl);
          }

          /* 4. what you can do about it, without opening anything */
          const acts=el('div','ov-tacts');
          const mkAct=function(cls,label,icon,fn){
            const b=el('button',cls);b.type='button';b.draggable=false;
            if(icon)b.innerHTML=icon;
            b.append(document.createTextNode(label));
            b.onclick=function(e){e.stopPropagation();fn();};
            acts.append(b);return b;
          };
          mkAct('','Edit','<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" '+
            'stroke-linejoin="round"><path d="M11.2 2.4 13.6 4.8 5.6 12.8 2.4 13.6l.8-3.2 8-8Z"/></svg>',
            async function(){
              try{sel=await api('/jots/'+j.id);render();}catch(e){toast(e.message,'err');}
            });
          /* Snooze pushes `due` to tomorrow morning - the same "reschedule, don't track a snoozed
             state" rule the dialog's snooze buttons follow. One click, no submenu: the dialog is
             right there for an exact time. */
          mkAct('','Snooze','<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" '+
            'stroke-linecap="round" stroke-linejoin="round"><circle cx="8" cy="8" r="6.2"/>'+
            '<path d="M8 4.6V8l2.4 1.6"/></svg>',
            async function(){
              const prev=tagValue(j.tags,'due:');
              const d=new Date(Date.now()+86400000);d.setHours(9,0,0,0);
              try{
                const updated=await setDue(j,toLocalInputValue(d));
                toast('Snoozed to tomorrow 9:00','ok',async function(){
                  try{await setDue(updated,prev||'');toast('Snooze undone');render();}
                  catch(err){toast(err.message,'err');}
                });
                render();
              }catch(e){toast(e.message,'err');}
            });
          acts.append(completeBtn(false,async function(){
            try{
              const updated=await toggleDone(j,true);
              toast('Marked completed','ok',async function(){
                try{await toggleDone(updated,false);toast('Restored');render();}
                catch(err){toast(err.message,'err');}
              });
              render();
            }catch(e){toast(e.message,'err');}
          }));
          r.append(acts);

          r.onclick=async function(){
            try{sel=await api('/jots/'+j.id);render();}
            catch(e){toast(e.message,'err');}
          };
          body.append(r);
        });
        if(list.length>6)col.append(el('div','note','+'+(list.length-6)+' more — open Search.'));
      });
    }

    /* ---- stat cards ---- */
    const grid=el('div','ov-grid');L.append(grid);
    const card=function(cls,icon,eyebrow,num,cap){
      const c=el('div','ov-card'+(cls?' '+cls:''));
      const top=el('div','top');
      const ic=el('div','ic');ic.innerHTML=OV_ICONS[icon];top.append(ic);
      top.append(el('div','eyebrow',eyebrow));c.append(top);
      c.append(el('div','num',String(num)));
      c.append(el('div','cap',cap));
      grid.append(c);
    };
    card('hi','dot','Active jots',health.jots??'—',
      (health.named||0)+' named · '+Math.max((health.jots||0)-(health.named||0),0)+' unnamed');
    card('','layers','Topics',topTags.length,'Bare-tag vocabulary');
    card('','hash','Tags in use',health.tags??'—','Including structural tags');
    card('','flag','Needs attention',sim.clusters.length,
      sim.clusters.length?'Tag groups that look like duplicates':'No drift detected');

    /* ---- distribution + signals ---- */
    const row1=el('div','ov-row');L.append(row1);

    const catCounts={};
    recent.jots.forEach(function(j){const c=catColorOf(j.tags);catCounts[c.name]=(catCounts[c.name]||0)+1;});
    const catList=Object.keys(catCounts).map(k=>[k,catCounts[k]]).sort((a,b)=>b[1]-a[1]);

    const distP=el('div','ov-panel');row1.append(distP);
    const dh=el('div','phead');const dh1=el('div');
    dh1.append(el('div','eyebrow','DISTRIBUTION'));dh1.append(el('h3',null,'Jots by topic'));
    dh.append(dh1);distP.append(dh);
    if(!catList.length){distP.append(el('div','empty','Nothing yet.'));}
    else{
      const maxC=catList[0][1];
      catList.slice(0,10).forEach(function(pair){
        const cat=hashCat(pair[0]);
        const b=el('div','ov-bar');b.style.setProperty('--cat','var('+cat.cssVar+')');
        const l=el('span','lbl',pair[0]);l.title=pair[0];b.append(l);
        const track=el('div','track');const fill=el('i','fill');
        fill.style.width=(100*pair[1]/maxC)+'%';track.append(fill);b.append(track);
        b.append(el('span','n',pair[1]));
        distP.append(b);
      });
    }

    const sigP=el('div','ov-panel');row1.append(sigP);
    const sh=el('div','phead');const sh1=el('div');
    sh1.append(el('div','eyebrow','SIGNALS'));sh1.append(el('h3',null,'Top Tags'));
    sh.append(sh1);sigP.append(sh);
    if(!topTags.length){sigP.append(el('div','empty','No tags yet.'));}
    else{
      const wrap=el('div','ov-tags');
      topTags.slice(0,8).forEach(function(t){
        const pill=el('div','ov-tagpill');
        pill.append(document.createTextNode('#'+t.tag));
        pill.append(el('b',null,t.count));
        pill.onclick=function(){view='search';activeTags=new Set([t.tag]);drawNav();render();};
        wrap.append(pill);
      });
      sigP.append(wrap);
    }

    /* ---- activity + store health ---- */
    const row2=el('div','ov-row');L.append(row2);

    const actP=el('div','ov-panel');row2.append(actP);
    const ah=el('div','phead');const ah1=el('div');
    ah1.append(el('div','eyebrow','ACTIVITY'));ah1.append(el('h3',null,'Recently changed'));
    ah.append(ah1);actP.append(ah);
    if(!recent.jots.length){
      actP.append(el('div','empty','No jots yet. Write one from Search.'));
    }else{
      const list=el('div','ov-activity');actP.append(list);
      recent.jots.slice(0,8).forEach(function(j){
        const cat=catColorOf(j.tags);
        const r=el('div','ov-arow');r.style.setProperty('--cat','var('+cat.cssVar+')');
        r.append(el('i','ov-adot'));
        const mid=el('div','ov-amid');
        mid.append(el('div','ov-atitle',j.name||(j.summary||'').slice(0,80)||'(untitled)'));
        mid.append(el('div','ov-asub',cat.name+' · '+(j.editor||'user')));
        r.append(mid);
        r.append(el('span','ov-awhen',ago(j.updated||j.id)));
        r.onclick=async function(){
          try{sel=await api('/jots/'+j.id);render();}
          catch(e){toast(e.message,'err');}
        };
        list.append(r);
      });
    }

    const healthP=el('div','ov-panel ov-health');row2.append(healthP);
    const hh=el('div','phead');const hh1=el('div');
    hh1.append(el('div','eyebrow','STORE HEALTH'));
    hh1.append(el('h3',null,p.enabled?'Persisted to disk':'RAM only'));
    hh.append(hh1);healthP.append(hh);
    healthP.append(el('div','lead',p.enabled?
      'WAL plus periodic snapshots - every write here survives a restart.':
      'Everything is lost on restart. Start Loom without --no-persist.'));
    const vr=function(k,v){const r=el('div','vrow');r.append(el('span',null,k));
      r.append(el('span','n',v));healthP.append(r);};
    vr('Jots',health.jots);
    vr('Tag vocabulary',health.tags);
    if(p.enabled){vr('WAL bytes',(p.wal_bytes/1024).toFixed(1)+' KB');vr('Snapshots',p.snapshots);}
    vr('Mutations this run',health.mutations);
  }catch(e){L.append(el('div','note bad',e.message));}

  clearInterval(window.__rt);
  window.__rt=setInterval(function(){if(view==='dashboard'&&!sel)render();},15000);
}

/* ---------- tags ---------- */
async function viewTags(target){
  const L=target;
  try{
    const both=await Promise.all([api('/tags?reserved=1'),api('/tags/similar')]);
    const tags=both[0],sim=both[1];

    L.append(el('div','sect','Drift — tags that look like variants of each other'));
    if(!sim.clusters.length){
      L.append(el('div','note good','No drift detected. The vocabulary is coherent.'));
    }else{
      sim.clusters.forEach(function(members){
        const c=el('div','cluster');
        c.append(el('div','lead',
          'These '+members.length+' tags are probably the same idea written differently.'));
        const r=el('div','row');
        members.forEach(m=>r.append(el('span','tag',m)));
        c.append(r);
        const act=el('div','row');act.style.marginTop='10px';
        const keep=el('select');keep.style.width='auto';
        members.forEach(function(m){const o=el('option',null,'keep “'+m+'”');
                                    o.value=m;keep.append(o);});
        const b=el('button','btn tiny primary','Merge');
        b.onclick=async function(){
          const to=keep.value,from=members.filter(m=>m!==to);
          if(!confirm('Merge '+from.join(', ')+' into “'+to+'”?\n\n'+
                      'This rewrites every jot carrying them. It cannot be undone.'))return;
          try{
            const res=await api('/tags/merge',{method:'POST',
              headers:{'Content-Type':'application/json'},
              body:JSON.stringify({from:from,to:to})});
            toast('Merged — '+res.changed+' jots rewritten');
            allTags=(await api('/tags')).tags;await refreshStats();render();
          }catch(e){toast(e.message,'err');}
        };
        act.append(keep,b);c.append(act);L.append(c);
      });
    }

    L.append(el('div','sect','Vocabulary — '+tags.count+' tags'));
    if(!tags.tags.length){
      L.append(el('div','empty','Nothing tagged yet.'));
    }else{
      let max=1;tags.tags.forEach(t=>{if(t.count>max)max=t.count;});
      tags.tags.forEach(function(s){
        const row=el('div','vrow');
        const fill=el('i','fill');fill.style.width=(100*s.count/max)+'%';row.append(fill);
        row.append(el('span',tagClass(s.tag),s.tag));
        row.append(el('span','n',s.count));
        const w=el('span','when',ago(s.last));w.title='last used '+stamp(s.last);row.append(w);
        row.onclick=function(){view='search';activeTags=new Set([s.tag]);drawNav();render();};
        L.append(row);
      });
    }
  }catch(e){L.append(el('div','note bad',e.message));}
}

/* ---------- health ---------- */
async function viewHealth(target){
  const L=target;
  try{
    const s=await api('/stats');const p=s.persistence||{};
    const group=function(title,rows){
      L.append(el('div','sect',title));
      rows.forEach(function(kv){
        const r=el('div','vrow');r.style.cursor='default';
        r.append(el('span',null,kv[0]));r.append(el('span','n',kv[1]));L.append(r);
      });
    };
    group('Store',[['Jots',s.jots],['Named',s.named],['Tag vocabulary',s.tags],
      ['Distinct terms',s.terms],['Pending links',s.pending_links],['Editors',s.editors],
      ['Mutations this run',s.mutations]]);
    if(s.oldest)group('Span',[['Oldest',stamp(s.oldest)],['Newest',stamp(s.newest)]]);

    if(!p.enabled){
      L.append(el('div','sect','Durability'));
      L.append(el('div','note bad',
        'Running in RAM only — everything is lost on restart. Start without --no-persist.'));
    }else{
      group('Durability',[['WAL bytes',p.wal_bytes],['Lines appended',p.appended],
        ['Fsyncs',p.synced],['Queue depth',p.queued],['Snapshots',p.snapshots]]);
      const row=el('div','row');row.style.marginTop='16px';
      const snap=el('button','btn tiny','Snapshot now');
      snap.onclick=async function(){try{const r=await api('/admin/snapshot',{method:'POST'});
        toast('Snapshot wrote '+r.records+' records');render();}
        catch(e){toast(e.message,'err');}};
      const fl=el('button','btn tiny','Flush WAL');
      fl.onclick=async function(){try{await api('/admin/flush',{method:'POST'});toast('Flushed');}
        catch(e){toast(e.message,'err');}};
      row.append(snap,fl);L.append(row);
    }

    L.append(el('div','sect','Endpoints'));
    L.append(el('div','note','REST at /jots · MCP at /mcp — connect an agent with: '+
      'claude mcp add --transport http loom '+location.origin+'/mcp'));
  }catch(e){L.append(el('div','note bad',e.message));}
}

/* ---------- history ----------
   Reads GET /history and offers the two things the log is for: putting a version back, and - when
   something was written down that should never have been - marking jots for a purge.

   RESTORE AND PURGE ARE DELIBERATELY UNALIKE HERE. Restore is one click with an undo toast, because
   it is reversible: restoring the wrong version just adds another entry you can restore past. Purge
   takes a checkbox, a reason, a dialog and then a procedure carried out with the service stopped,
   because it is the one operation in Loom that nothing can walk back - including the log this page
   is showing you. The asymmetry in the UI is the point. */
let histFilterID=0;
let purgeSel=new Set();

function histOpClass(e,bFirstForJot){
  if(e.op==='del')return 'del';
  return bFirstForJot?'new':'put';
}

async function viewHistory(target){
  const L=target;

  /* A pending request outranks everything else on the page: it means somebody has already asked
     for an irreversible thing and it is sitting there waiting to be carried out or cancelled. */
  try{
    const pending=await api('/purge/request');
    const b=el('div','purgebanner');
    b.append(el('h3',null,'A purge is pending'));
    const p=el('p');
    p.append(document.createTextNode(
      pending.jots.length+' jot'+(pending.jots.length===1?'':'s')+' marked by '+
      (pending.requested_by||'someone')+' on '+stamp(pending.created)+'. '+
      'Nothing has been erased yet - the request is inert until somebody runs the offline purge.'));
    b.append(p);
    if(pending.reason)b.append(el('p',null,'Reason: '+pending.reason));
    const row=el('div','row');
    const show=el('button','btn tiny','Show instructions');
    show.onclick=function(){showPurgeInstructions(pending.instructions);};
    const cancel=el('button','btn tiny danger','Cancel the request');
    cancel.onclick=async function(){
      try{await api('/purge/request',{method:'DELETE'});toast('purge request cancelled');render();}
      catch(e){toast(e.message,'bad');}
    };
    row.append(show,cancel);b.append(row);
    L.append(b);
  }catch(e){/* 404 is the normal case - no request pending */}

  const head=el('div','row');head.style.marginBottom='14px';
  head.append(el('div','sect','Every change, newest first'));
  if(histFilterID){
    const clear=el('button','btn tiny','Showing one jot - show all');
    clear.onclick=function(){histFilterID=0;render();};
    head.append(clear);
  }
  L.append(head);

  let data;
  try{
    data=await api('/history?limit=200'+(histFilterID?'&id='+histFilterID:''));
  }catch(e){
    L.append(el('div','note bad',e.message));
    return;
  }

  if(!data.entries.length){
    L.append(el('div','note','Nothing recorded yet. Every create, edit and delete lands here.'));
    return;
  }

  /* Oldest-first pass to work out which entry is a jot's FIRST, so it can read "created" rather
     than "edited" - the log itself does not distinguish them, because a put is a put. */
  const seen=new Set();
  const firstSeq=new Set();
  for(let i=data.entries.length-1;i>=0;i--){
    const e=data.entries[i];
    if(e.op!=='del'&&!seen.has(e.id)){seen.add(e.id);firstSeq.add(e.seq);}
  }

  let lastDay='';
  data.entries.forEach(function(e){
    const day=dayKey(e.at);
    if(day!==lastDay){lastDay=day;L.append(el('div','daybar',dayLabel(e.at)));}

    const row=el('div','hrow'+(purgeSel.has(e.id)?' sel':''));
    const cls=histOpClass(e,firstSeq.has(e.seq));
    row.append(el('div','hop '+cls,cls==='del'?'deleted':(cls==='new'?'created':'edited')));

    const main=el('div','hmain');
    const name=el('div','hname'+(e.op==='del'?' gone':''),e.name||'(unnamed)');
    main.append(name);
    if(e.summary)main.append(el('div','hsum',e.summary));
    /* Clicking the row filters to that jot - "what else happened to this one" is the question you
       always have next. */
    main.style.cursor='pointer';
    main.onclick=function(){histFilterID=e.id;render();};
    row.append(main);

    const meta=el('div','hmeta');
    meta.append(el('b',null,e.editor||'user'));
    meta.append(document.createTextNode(ago(e.at)));
    row.append(meta);

    const acts=el('div','hacts');
    const rb=el('button','btn tiny',e.op==='del'?'Undo delete':'Restore');
    rb.title=e.op==='del'
      ?'Put this jot back as it was immediately before the delete'
      :'Make this jot look like it did at this point';
    rb.onclick=async function(){
      rb.disabled=true;
      try{
        const r=await api('/history/restore',
          {method:'POST',headers:{'Content-Type':'application/json'},
           body:JSON.stringify({seq:e.seq})});
        toast(r.undid_delete?'restored '+(r.jot.name||'the jot')
                            :'restored '+(r.jot.name||'the jot')+' to that version');
        render();
      }catch(err){toast(err.message,'bad');rb.disabled=false;}
    };
    acts.append(rb);

    const cb=el('input');cb.type='checkbox';cb.checked=purgeSel.has(e.id);
    cb.title='Mark this jot for purging';
    cb.onchange=function(){
      if(cb.checked)purgeSel.add(e.id);else purgeSel.delete(e.id);
      render();
    };
    acts.append(cb);
    row.append(acts);
    L.append(row);
  });

  if(data.total>data.entries.length)
    L.append(el('div','note','Showing the most recent '+data.entries.length+' of '+data.total+
      ' entries in memory. Older ones are still in loom.history on disk.'));

  if(purgeSel.size){
    const bar=el('div','purgebar');
    bar.append(document.createTextNode(
      purgeSel.size+' jot'+(purgeSel.size===1?'':'s')+' marked. Purging erases every version of '+
      'them from the snapshot, the WAL and this log - it cannot be undone.'));
    const go=el('button','btn tiny danger','Request purge...');
    go.onclick=openPurge;
    const clr=el('button','btn tiny ghost','Clear');
    clr.onclick=function(){purgeSel.clear();render();};
    bar.append(go,clr);
    L.append(bar);
  }
}

function showPurgeInstructions(sText){
  $('#purge-step').textContent=sText;
  $('#purge-ask').style.display='none';
  $('#purge-done').style.display='';
  /* Reached two ways: from the pending banner with the dialog CLOSED, and from Write-the-request
     with it already open on the ask step. showModal() on an open dialog throws InvalidStateError,
     so the second path has to swap panels without reopening. */
  const d=$('#purge-dialog');
  if(!d.open)d.showModal();
}

function openPurge(){
  $('#purge-reason').value='';
  $('#purge-count').textContent=purgeSel.size+' jot'+(purgeSel.size===1?'':'s');
  $('#purge-ask').style.display='';
  $('#purge-done').style.display='none';
  $('#purge-dialog').showModal();
  $('#purge-reason').focus();
}

$('#purge-cancel').addEventListener('click',()=>$('#purge-dialog').close());
$('#purge-close').addEventListener('click',function(){$('#purge-dialog').close();render();});
$('#purge-copy').addEventListener('click',function(){
  navigator.clipboard.writeText($('#purge-step').textContent)
    .then(()=>toast('instructions copied - paste them to an agent'))
    .catch(()=>toast('could not copy','bad'));
});
$('#purge-submit').addEventListener('click',async function(){
  const reason=$('#purge-reason').value.trim();
  if(!reason){toast('say why - the confirmation step depends on it','warn');return;}
  try{
    const r=await api('/purge/request',
      {method:'POST',headers:{'Content-Type':'application/json'},
       body:JSON.stringify({ids:Array.from(purgeSel),reason:reason})});
    purgeSel.clear();
    showPurgeInstructions(r.instructions);
  }catch(e){toast(e.message,'bad');}
});

/* ---------- detail / editor ---------- */
function renderDetail(){
  const dlg=$('#detail-dialog');
  if(!sel){
    if(dlg.open)dlg.close();
    return;
  }
  if(!dlg.open)dlg.showModal();
  const P=$('#detail');P.innerHTML='';
  const isNew=!!sel.__new;
  const W=el('div','dwrap');P.append(W);

  /* Expanded/force-todo state remembers across re-renders of the SAME jot (e.g. after Save) but
     resets the moment a different jot is opened - carrying "I had it expanded" or "I turned this
     into a TODO" over to the next unrelated click would defeat the point of both. */
  const key=isNew?'__new':sel.id;
  if(key!==detailOpenedKey){detailExpanded=false;detailForceTodo=false;detailOpenedKey=key;}

  /* ---- header: slug reads as the title on the left, id sits quietly at the right ---- */
  const h=el('div','dhead');
  h.append(el('h3',null,isNew?'New jot':(sel.name||'Edit jot')));
  if(!isNew)h.append(el('span','did','#'+sel.id));
  W.append(h);

  const f={};
  const sect=function(legend,host){
    const d=el('div','dsect');
    if(legend)d.append(el('div','dlegend',legend));
    (host||W).append(d);return d;
  };
  /* every field is its own .fld block so short fields can be paired two-up in a .frow when
     the panel is wide - a label and its input have to travel together through the grid. */
  /* A null label means the enclosing section's legend already names the field - repeating it
     immediately underneath is noise. The hint still gets a line of its own. */
  const field=function(key,label,tag,hint,host){
    const box=el('div','fld');(host||W).append(box);
    if(label||hint){
      const l=el('label');
      if(label)l.append(document.createTextNode(label));
      if(hint)l.append(el('u',null,hint));
      box.append(l);
    }
    const e=el(tag||'input');e.value=(sel[key]===undefined||sel[key]===null)?'':sel[key];
    e.setAttribute('data-k',key);
    if(tag==='textarea')e.rows=(key==='text')?9:2;
    box.append(e);f[key]=e;return e;
  };
  const frow=function(host){const d=el('div','frow');(host||W).append(d);return d;};

  /* ---- minimal core: what a TODO actually needs, always visible ---- */
  /* Priority/due only show for jots that are already TODO-ish (see isTodo()) - otherwise every
     ordinary jot's dialog got task controls it had no use for. "Make this a TODO" is the explicit
     opt-in for a plain jot; it doesn't touch the server by itself, it just reveals the fields so
     they can be set before the next Save. */
  const showTask=isTodo(sel)||detailForceTodo;
  /* pr is a plain {value} holder rather than the <select> it used to be, so Save reads priority
     the same way regardless of which control drew it. */
  const pr={value:showTask?(tagValue(sel.tags,'priority:')||''):''};
  let dueIn;

  /* ---- priority leads: highest-traffic field on a TODO, and the one that moves a card ---- */
  if(showTask){
    const prBlock=el('div','dbare');W.append(prBlock);
    prBlock.append(el('div','dlegend','priority'));
    const prRow=el('div','prio');prBlock.append(prRow);
    const rname='prio-'+key;
    [['high','High','p-high'],['normal','Normal','p-normal'],
     ['low','Low','p-low'],['','Clear','p-none']].forEach(function(o){
      const lab=el('label','prchip '+o[2]);
      const rb=el('input');rb.type='radio';rb.name=rname;rb.value=o[0];
      rb.checked=(pr.value===o[0]);
      /* Radios do the unsetting for free: "Clear" is just the option whose value is empty, so
         picking it deselects the other three exactly the way picking High does. The chip's look
         follows :checked in CSS, so there's no class to keep in sync here. */
      rb.onchange=function(){pr.value=o[0];};
      lab.append(rb,el('span',null,o[1]));
      prRow.append(lab);
    });
  }else{
    /* Not a ghost: it's the only control between the header and the first card, so on the
       dialog's bare ground a borderless button read as a caption rather than something to press. */
    const mk=el('button','btn tiny dmktodo','Make this a TODO');
    mk.type='button';
    mk.onclick=function(){detailForceTodo=true;renderDetail();};
    W.append(mk);
  }

  /* ---- summary, with the way into everything else in its bottom-right corner ---- */
  const sumSect=sect('summary');
  field('summary',null,'textarea','the main point of this jot',sumSect);
  const sumFoot=el('div','dsectfoot');sumSect.append(sumFoot);

  /* ---- details, metadata: revealed together ----
     The toggle exists TWICE - once in the summary box, once in the details box - and only the one
     belonging to the currently-visible arrangement is shown. That's what lets the control sit in
     the bottom-right corner of whichever box it closes, without re-rendering the dialog to move
     it: a re-render would throw away whatever had been typed and not yet saved. */
  const detSect=sect('details');
  const dta=el('textarea');dta.setAttribute('data-k','text');dta.value=sel.text||'';dta.rows=9;
  detSect.append(dta);f.text=dta;
  const detFoot=el('div','dsectfoot');detSect.append(detFoot);

  const metaSect=sect('metadata');
  const idRow=frow(metaSect);
  field('name','slug',null,'optional - makes this jot addressable',idRow);
  /* Editor used to appear twice - once as an editable field and once as a read-only line in the
     metadata block. This is the editable one; the duplicate is gone. */
  field('editor','editor',null,null,idRow);

  const TASK_TAG=/^(priority|due):/;
  const metaRow=frow(metaSect);
  const tg=field('tags','tags',null,'comma separated',metaRow);
  tg.value=(sel.tags||[]).filter(t=>!TASK_TAG.test(t)).join(', ');
  const lk=field('links','links',null,'ids or slugs',metaRow);
  lk.value=(sel.links||[]).concat(sel.pending||[]).join(', ');

  if(!isNew&&(sel.pending||[]).length)
    metaSect.append(el('div','note','Unresolved: '+sel.pending.join(', ')+
      ' — these connect themselves when a jot takes that slug.'));

  if(!isNew){
    const m=el('div','dmeta');
    const line=function(k,v){const d=el('div');d.append(el('i',null,k));
      d.append(el('span',null,v));m.append(d);};
    line('created',stamp(sel.id));
    if(sel.updated)line('edited',stamp(sel.updated));
    metaSect.append(m);
  }

  const mkToggle=function(host,label){
    const b=el('button','btn tiny ghost',label);b.type='button';
    b.onclick=function(){detailExpanded=!detailExpanded;applyExpanded();};
    host.append(b);return b;
  };
  mkToggle(sumFoot,'More details ▾');
  mkToggle(detFoot,'Fewer details ▲');
  const applyExpanded=function(){
    detSect.hidden=!detailExpanded;
    metaSect.hidden=!detailExpanded;
    sumFoot.hidden=detailExpanded;
    /* Wider only while the details field is on screen - see dialog#detail-dialog.wide. */
    dlg.classList.toggle('wide',detailExpanded);
  };
  applyExpanded();

  /* Due is its own subsection below the fields: WHEN something is due is a separate decision from
     what it says and from how much it matters, and pairing it with priority in one row made the
     two look like halves of a single setting. */
  if(showTask){
    const dueSect=sect('schedule');
    dueSect.append(el('label',null,'due'));
    dueIn=el('input','dueinput');dueIn.type='datetime-local';
    const dv=tagValue(sel.tags,'due:');
    dueIn.value=dv?(dv.length>10?dv:dv+'T00:00'):'';
    dueSect.append(dueIn);

    /* Snooze is just "reschedule due to later" - there's no separate snoozed state to track, so
       these apply immediately (PATCH the due tag, update the field) rather than staging a value
       for the next unrelated Save. Only shown for an EXISTING jot with something to reschedule. */
    if(!isNew){
      const snBtns=el('div','snbtns');dueSect.append(snBtns);
      const applyDue=async function(val){
        try{
          sel=await setDue(sel,val);
          dueIn.value=val?(val.length>10?val:val+'T00:00'):'';
          toast(val?'Rescheduled':'Due date cleared');
        }catch(e){toast(e.message,'err');}
      };
      const mkSnBtn=function(label,fn){
        const b=el('button','btn tiny ghost',label);b.type='button';
        b.onclick=function(e){e.preventDefault();applyDue(fn());};
        snBtns.append(b);
      };
      mkSnBtn('Snooze +1h',()=>toLocalInputValue(new Date(Date.now()+3600000)));
      mkSnBtn('Snooze to tomorrow',()=>{const d=new Date(Date.now()+86400000);d.setHours(9,0,0,0);
        return toLocalInputValue(d);});
      mkSnBtn('Snooze +1 week',()=>toLocalInputValue(new Date(Date.now()+7*86400000)));
      mkSnBtn('Clear due',()=>'');
    }
  }

  const act=el('div','actions');
  const save=el('button','btn primary',isNew?'Create':'Save');
  save.onclick=async function(){
    const tags=tg.value.split(',').map(s=>s.trim()).filter(Boolean);
    if(pr&&pr.value)tags.push('priority:'+pr.value);
    if(dueIn&&dueIn.value)tags.push('due:'+dueIn.value);
    const body={text:f.text.value,name:f.name.value,summary:f.summary.value,
      editor:f.editor.value,
      tags:tags,
      links:lk.value.split(',').map(s=>s.trim()).filter(Boolean)};
    try{
      let r;
      if(isNew){
        r=await api('/jots',{method:'POST',headers:{'Content-Type':'application/json'},
                             body:JSON.stringify(body)});
      }else{
        /* expect_updated is the multi-agent guard: if anything changed since this panel
           loaded, the server answers 409 instead of losing the other write. */
        const exp=sel.updated||sel.id;
        r=await api('/jots/'+sel.id+'?expect_updated='+exp,
          {method:'PATCH',headers:{'Content-Type':'application/json'},
           body:JSON.stringify(body)});
      }
      if(r.warnings&&r.warnings.length)toast(r.warnings[0],'warn');
      else toast(isNew?'Created':'Saved');
      /* Save closes the dialog - it's the "I'm done with this" action, not a checkpoint to keep
         editing from. Re-open it from the list/panel to keep going. */
      sel=null;
      allTags=(await api('/tags')).tags;await refreshStats();render();
    }catch(e){
      if(e.status===409)
        toast('Conflict — someone else changed this jot. Close and reopen before saving.','err');
      else toast(e.message,'err');
    }
  };
  act.append(save);

  if(!isNew){
    const rel=el('button','btn tiny','Linked');
    let linkedBox=null;
    rel.onclick=async function(){
      try{
        const r=await api('/jots/'+sel.id+'/links?depth=2');
        if(linkedBox)linkedBox.remove();
        linkedBox=el('div');W.append(linkedBox);
        linkedBox.append(el('div','sect','Linked — 2 hops, both directions'));
        if(!r.jots.length)linkedBox.append(el('div','empty','Nothing links here.'));
        const g=el('div','cardgrid');
        r.jots.forEach(j=>g.append(jotCard(j,0,[])));
        linkedBox.append(g);
      }catch(e){toast(e.message,'err');}
    };
    const del=el('button','btn tiny danger','Delete');
    del.onclick=async function(){
      if(!confirm('Delete this jot permanently? There is no undo.'))return;
      try{await api('/jots/'+sel.id,{method:'DELETE'});toast('Deleted');
        sel=null;
        await refreshStats();render();}
      catch(e){toast(e.message,'err');}
    };
    act.append(rel,del);
  }
  const close=el('button','btn tiny ghost','Close');
  close.onclick=function(){sel=null;render();};
  act.append(close);

  /* Complete rides the actions row at the bottom right - same button, same place it occupies on
     a reminder card. Toggling it touches ONLY `status:done`: priority, due and everything else
     survive, which is what makes reopening a completed TODO put it back exactly as it was. */
  if(showTask&&!isNew){
    const wasDone=isDone(sel);
    act.append(completeBtn(wasDone,async function(){
      try{
        sel=await toggleDone(sel,!wasDone);
        const updated=sel;
        toast(wasDone?'Reopened':'Marked completed','ok',async function(){
          try{
            sel=await toggleDone(updated,wasDone);
            toast(wasDone?'Marked completed':'Reopened');
            await refreshStats();render();
          }catch(err){toast(err.message,'err');}
        });
        await refreshStats();render();
      }catch(e){toast(e.message,'err');}
    }));
  }
  W.append(act);

  P.onkeydown=function(e){
    if((e.metaKey||e.ctrlKey)&&e.key==='Enter'){e.preventDefault();save.click();}
  };
}

/* ---------- shell ---------- */
/* Each view builds into a detached staging container and only gets swapped into #list once every
   await inside it has resolved - the old approach cleared #list synchronously and repopulated it
   across several awaited fetches, so the panel sat visibly blank for the round trip every time
   (glaringly so on the 15s dashboard auto-refresh). One atomic swap means there's never a blank
   frame; scroll position is kept only when the view didn't change, since jumping to the same
   scroll offset after switching views would be its own kind of glitch. */
let lastRenderedView=null;
async function render(){
  const D=el('div');
  if(view==='dashboard')await viewDashboard(D);
  else if(view==='search')await viewSearch(D);
  else if(view==='tags')await viewTags(D);
  else if(view==='history')await viewHistory(D);
  else await viewHealth(D);
  const L=$('#list');
  const keepScroll=(view===lastRenderedView);
  const top=L.scrollTop;
  /* Every view lands inside the capped, centered column - done here rather than in each view so
     a new view can't forget to do it and quietly stretch to 2560px. */
  const wrap=el('div','contentwrap');
  wrap.append(...D.childNodes);
  L.replaceChildren(wrap);
  L.scrollTop=keepScroll?top:0;
  lastRenderedView=view;
  const af=L.querySelector('[data-autofocus]');
  if(af){af.focus();if(af.setSelectionRange)af.setSelectionRange(af.value.length,af.value.length);}
  renderDetail();
}

(function(){
  /* Always an explicit choice now, not "follow the OS, unless overridden" - that's what made the
     default and the darkest explicit palette collapse into near-duplicates. First visit still
     picks a sane starting side (light vs dark) from the OS, but from then on it's just whatever
     was last picked - one of the named values in #palette-select, and an unknown one stored by an
     older build simply lands on :root's Paper rather than erroring. */
  const KEY='loom-palette';let saved=null;try{saved=localStorage.getItem(KEY);}catch(e){}
  const initial=saved||(matchMedia('(prefers-color-scheme:dark)').matches?'midnight':'paper');
  document.documentElement.setAttribute('data-palette',initial);
  const sel=$('#palette-select');sel.value=initial;
  sel.addEventListener('change',()=>{
    const v=sel.value;
    document.documentElement.setAttribute('data-palette',v);
    try{localStorage.setItem(KEY,v);}catch(e){}
  });
})();

/* ---------- access list ----------
   The dialog edits a LOCAL COPY and sends the whole list on Save. Nothing here mutates the running
   list a rule at a time: a half-applied allow list is a security control nobody can reason about,
   and it is also the state you would be left in if the page failed between two of the requests.

   THE SERVER OWNS THE LOCKOUT RULES, not this page. The dashboard offers "Add this address" and
   marks whichever rule covers you, but the refusal to apply a list that excludes you comes back
   from PUT /acl - so curl gets the same protection, and a future front end cannot forget it. The
   Force button below is how you overrule it, and it is deliberately not the primary action. */
const ACL_LOCKED='<path d="M8 1.6 2.6 3.9v3.7c0 3.1 2.2 6 5.4 6.8 3.2-.8 5.4-3.7 5.4-6.8V3.9Z"/>'+
                 '<path d="M6.3 7.6V6.4a1.7 1.7 0 0 1 3.4 0v1.2"/>'+
                 '<rect x="5.6" y="7.6" width="4.8" height="3.6" rx="0.8"/>';
const ACL_OPEN  ='<path d="M8 1.6 2.6 3.9v3.7c0 3.1 2.2 6 5.4 6.8 3.2-.8 5.4-3.7 5.4-6.8V3.9Z"/>'+
                 '<path d="M6.3 7.6V6.4a1.7 1.7 0 0 1 3.3-.4"/>'+
                 '<rect x="5.6" y="7.6" width="4.8" height="3.6" rx="0.8"/>';
let aclNow={enabled:false,entries:[],caller:'',caller_is_loopback:false};
let aclDraft={enabled:false,entries:[]};

/* Reflects the LIVE list, not the draft - the rail has to keep telling the truth while the dialog
   is open with unsaved changes in it. */
function drawShield(){
  const b=$('#acl-btn'),i=$('#acl-icon');
  if(!b)return;
  b.classList.toggle('on',!!aclNow.enabled);
  i.innerHTML=aclNow.enabled?ACL_LOCKED:ACL_OPEN;
  b.title=aclNow.enabled
    ?('Access list on - '+aclNow.entries.length+' rule'+(aclNow.entries.length===1?'':'s'))
    :'Access list off - Loom answers any address';
}

async function refreshAcl(){
  try{aclNow=await api('/acl');}catch(e){/* an unreachable server is already said by the dot */}
  drawShield();
}

function aclCovers(rule,caller){
  /* Deliberately NOT a reimplementation of the server's matcher - that would be a second parser to
     keep in step with the first. This only has to decide whether to draw a badge, so it answers the
     two cases a person actually types and says nothing about the rest. */
  if(!caller)return false;
  if(rule===caller)return true;
  const m=/^(\d+\.\d+\.\d+)\.\d+\/24$/.exec(rule);
  if(m)return caller.replace(/^::ffff:/,'').startsWith(m[1]+'.');
  return false;
}

function renderAclRules(){
  const R=$('#acl-rules');R.innerHTML='';
  const caller=(aclDraft.entries.length?aclNow.caller:aclNow.caller)||'';
  if(!aclDraft.entries.length){
    R.append(el('div','aclempty','No addresses listed. Add one before turning the list on.'));
  }
  aclDraft.entries.forEach(function(e,i){
    const mine=aclCovers(e.rule,caller);
    const row=el('div','aclrule'+(mine?' self':''));
    row.append(el('span','r',e.rule));
    if(mine)row.append(el('span','me','you'));
    row.append(el('span','n',e.note||''));
    const x=el('button',null,'×');
    x.title='Remove';
    x.onclick=function(){aclDraft.entries.splice(i,1);renderAclRules();};
    row.append(x);
    R.append(row);
  });
  $('#acl-count').textContent=aclDraft.entries.length+' rule'+
    (aclDraft.entries.length===1?'':'s');
}

function aclAdd(sRule,sNote){
  const r=(sRule||'').trim();
  if(!r)return;
  if(aclDraft.entries.some(e=>e.rule===r)){toast('already listed','warn');return;}
  aclDraft.entries.push({rule:r,note:(sNote||'').trim()});
  renderAclRules();
}

async function openAcl(){
  try{aclNow=await api('/acl');}
  catch(e){toast('could not read the access list: '+e.message,'bad');return;}
  aclDraft={enabled:!!aclNow.enabled,entries:(aclNow.entries||[]).map(e=>({rule:e.rule,note:e.note||''}))};
  $('#acl-enabled').checked=aclDraft.enabled;
  $('#acl-caller').textContent=aclNow.caller+(aclNow.caller_is_loopback?' (this machine)':'');
  /* Loopback is always allowed, so offering to add it would list a rule that changes nothing. */
  $('#acl-addme').style.display=aclNow.caller_is_loopback?'none':'';
  $('#acl-err').innerHTML='';
  $('#acl-input').value='';$('#acl-note').value='';
  renderAclRules();
  drawShield();
  $('#acl-dialog').showModal();
}

async function saveAcl(bForce){
  $('#acl-err').innerHTML='';
  const body={enabled:$('#acl-enabled').checked,entries:aclDraft.entries};
  try{
    await api('/acl'+(bForce?'?force=1':''),
              {method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    $('#acl-dialog').close();
    await refreshAcl();
    toast(aclNow.enabled?'access list on - '+aclNow.entries.length+' rule'+
          (aclNow.entries.length===1?'':'s'):'access list off');
  }catch(e){
    const box=el('div','aclerr');
    box.append(el('div',null,e.message));
    /* Only a refusal the server says is overridable gets an override button. A 500 does not. */
    if(e.status===403||e.status===400){
      const row=el('div','row');
      const f=el('button','btn tiny danger','Apply anyway');
      f.onclick=function(){saveAcl(true);};
      const c=el('button','btn tiny ghost','Back');
      c.onclick=function(){$('#acl-err').innerHTML='';};
      row.append(f);row.append(c);box.append(row);
    }
    $('#acl-err').innerHTML='';$('#acl-err').append(box);
  }
}

$('#acl-btn').addEventListener('click',openAcl);
$('#acl-cancel').addEventListener('click',()=>$('#acl-dialog').close());
$('#acl-save').addEventListener('click',()=>saveAcl(false));
$('#acl-add').addEventListener('click',function(){
  aclAdd($('#acl-input').value,$('#acl-note').value);
  $('#acl-input').value='';$('#acl-note').value='';$('#acl-input').focus();
});
$('#acl-input').addEventListener('keydown',function(e){
  if(e.key==='Enter'){e.preventDefault();$('#acl-add').click();}
});
$('#acl-addme').addEventListener('click',function(){
  aclAdd(aclNow.caller,'this machine');
});
$('#acl-dialog').addEventListener('click',function(e){if(e.target===this)this.close();});

$('#about-btn').addEventListener('click',()=>$('#about').showModal());
$('#about-close').addEventListener('click',()=>$('#about').close());
$('#about').addEventListener('click',function(e){if(e.target===this)this.close();});

/* Both open the same new-jot editor - a TODO is just a jot with task fields showing. New TODO
   pre-arms detailOpenedKey/detailForceTodo the way clicking "Make this a TODO" inside the dialog
   does, so those fields are already open on first paint instead of a second click to reveal them. */
$('#new-jot-btn').addEventListener('click',function(){
  sel={__new:true};
  render();
});
$('#new-todo-btn').addEventListener('click',function(){
  sel={__new:true};
  detailExpanded=false;detailForceTodo=true;detailOpenedKey='__new';
  render();
});

$('#agent-btn').addEventListener('click',function(){
  $('#agent-prompt-text').textContent=agentPrompt();
  $('#agent-dialog').showModal();
});
$('#agent-close').addEventListener('click',()=>$('#agent-dialog').close());
$('#agent-dialog').addEventListener('click',function(e){if(e.target===this)this.close();});

/* Esc and the backdrop both fire the dialog's native close - sel has to fall back in step so a
   later render() doesn't reopen it. The X button and in-panel Close button just call render()
   after clearing sel; this covers the two paths that don't. */
$('#detail-close-x').addEventListener('click',()=>$('#detail-dialog').close());
$('#detail-dialog').addEventListener('click',function(e){if(e.target===this)this.close();});
$('#detail-dialog').addEventListener('close',function(){if(sel){sel=null;render();}});
$('#agent-copy').addEventListener('click',function(){copyText(agentPrompt(),$('#agent-copy'));});

/* ---------- reminder notifications ----------
   Runs independently of whatever view is on screen - a reminder due while you're in Search
   should still fire. Two events per due TODO: "upcoming" (UPCOMING_LEAD_MS before) and "due now"
   (once the time passes), each fired at most once per due VALUE (loom-notified remembers which
   due timestamp it already fired for, so editing the due date re-arms it instead of staying
   silent, but a page reload doesn't re-fire the same one). Snoozing a jot reschedules its `due`
   tag to later, nothing more - that alone changes dueMs and naturally re-arms both events at the
   new time, so there's no separate suppression state to check here. */
const UPCOMING_LEAD_MS=15*60000;
const NOTIF_KEY='loom-notified';
const fmtLocal=d=>d.toLocaleString(undefined,
  {month:'short',day:'numeric',hour:'2-digit',minute:'2-digit'});
function loadNotified(){try{return JSON.parse(localStorage.getItem(NOTIF_KEY)||'{}');}catch(e){return{};}}
function saveNotified(m){try{localStorage.setItem(NOTIF_KEY,JSON.stringify(m));}catch(e){}}
function fireReminder(title,body,j,tag){
  try{
    const n=new Notification(title,{body:body,tag:tag,icon:location.origin+'/icon.png'});
    n.onclick=function(){
      window.focus();
      (async function(){try{sel=await api('/jots/'+j.id);renderDetail();}catch(e){}})();
      n.close();
    };
  }catch(e){/* Notification can throw in odd embed contexts - a missed reminder beats a crash */}
}
async function checkReminders(){
  if(!('Notification' in window)||Notification.permission!=='granted')return;
  let jots;
  try{jots=(await api('/jots?order=newest&limit=200&brief=1')).jots;}catch(e){return;}
  const notified=loadNotified(),now=Date.now();let changed=false;
  jots.forEach(function(j){
    if(isDone(j))return;
    const due=dueOf(j);if(!due)return;
    const dueMs=due.getTime(),key=String(j.id),rec=notified[key]||{};
    const label=j.name||(j.summary||j.text||'Reminder').slice(0,80);
    if(now>=dueMs-UPCOMING_LEAD_MS&&now<dueMs&&rec.upcomingFor!==dueMs){
      fireReminder('Upcoming: '+label,dueLabel(due)+' — '+fmtLocal(due),j,'loom-upcoming-'+key);
      rec.upcomingFor=dueMs;notified[key]=rec;changed=true;
    }
    if(now>=dueMs&&rec.dueFor!==dueMs){
      fireReminder('Due now: '+label,'Was due '+fmtLocal(due),j,'loom-due-'+key);
      rec.dueFor=dueMs;notified[key]=rec;changed=true;
    }
  });
  if(changed)saveNotified(notified);
}
(function(){
  const btn=$('#notif-btn'),label=$('#notif-label');
  const paint=function(){
    if(!('Notification' in window)){btn.classList.add('off');label.textContent='Unsupported';return;}
    if(Notification.permission==='granted'){btn.classList.add('on');btn.classList.remove('off');
      label.textContent='Reminders on';}
    else if(Notification.permission==='denied'){btn.classList.add('off');
      label.textContent='Reminders blocked';}
    else{btn.classList.remove('on','off');label.textContent='Enable reminders';}
  };
  paint();
  btn.addEventListener('click',async function(){
    if(!('Notification' in window))return;
    if(Notification.permission==='denied'){
      /* Once a browser has recorded "no," a page can never re-prompt for it - only the user can,
         from the browser's own permission UI. Best this button can do is say so, not sit there
         looking clickable with nothing to click. */
      toast('Reminders were blocked in the browser - click the icon in the address bar (or your '+
        'browser\'s site settings) to allow notifications, then reload.','warn');
      return;
    }
    if(Notification.permission!=='default')return;
    try{await Notification.requestPermission();}catch(e){}
    paint();
    if(Notification.permission==='granted')checkReminders();
  });
  setInterval(checkReminders,60000);
  if('Notification' in window&&Notification.permission==='granted')checkReminders();
})();

document.addEventListener('keydown',function(e){
  const typing=/^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement.tagName);
  if(e.key==='/'&&!typing){e.preventDefault();$('#navsearch-input').focus();}
  if(e.key==='Escape'){
    if(sel){sel=null;render();}
    else if(typing)document.activeElement.blur();
  }
});

(async function(){
  drawNav();
  try{allTags=(await api('/tags')).tags;}catch(e){}
  await refreshStats();
  await refreshAcl();
  await render();
  setInterval(refreshStats,4000);
})();
</script>
</body>
</html>
)HTML";
}
