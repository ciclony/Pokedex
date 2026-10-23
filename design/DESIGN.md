# Design reference: "Pokédex Prototype v2.0"

Source: Figma community file "Pokédex (Community)" by Ricardo Schiniegoski, based on PokeAPI.
https://www.figma.com/design/fnwkm9FzrEW9lDN7mQ8Pwk/Pok%C3%A9dex--Community-

Exports of the file are in `figma/`: the screens (`screens/`), the component sheet
(`components/`), the style guide (`style guide/`) and the overview (`overview/`).
The design targets a 360x640 phone screen. It is the reference for the CYD firmware
and for the planned Android app.

## Colors

| Role | Hex |
|---|---|
| Primary (identity) | `#DC0A2D` |
| Dark | `#212121` |
| Medium | `#666666` |
| Light | `#E0E0E0` |
| Background | `#EFEFEF` |
| White | `#FFFFFF` |

Pokémon types:

| Type | Hex | Type | Hex | Type | Hex |
|---|---|---|---|---|---|
| Bug | `#A7B723` | Fire | `#F57D31` | Ice | `#9AD6DF` |
| Dark | `#75574C` | Flying | `#A891EC` | Poison | `#A43E9E` |
| Dragon | `#7037FF` | Ghost | `#70559B` | Psychic | `#FB5584` |
| Electric | `#F9CF30` | Normal | `#AAA67F` | Rock | `#B69E31` |
| Fairy | `#E69EAC` | Grass | `#74CB48` | Steel | `#B7B9D0` |
| Fighting | `#C12239` | Ground | `#DEC16B` | Water | `#6493EB` |

## Typography (Poppins)

| Style | Weight | Size / line height |
|---|---|---|
| Headline | Bold | 24/32 px |
| Subtitle 1 | Bold | 14/16 px |
| Subtitle 2 | Bold | 12/16 px |
| Subtitle 3 | Bold | 10/16 px |
| Body 1 | Regular | 14/16 px |
| Body 2 | Regular | 12/16 px |
| Body 3 | Regular | 10/16 px |
| Caption | Regular | 8/12 px |

## Icons

Arrow back, chevron left/right, close (x), search, sort, ruler (height),
hash (# sort), underlined A (A–Z sort), weight bag, solid Pokéball (logo).
See `figma/components/Icons.png`.

## Components

- **Search bar:** white pill with an inner shadow, a red search icon and the placeholder "Search".
  While typing, it shows the text, a cursor and a red "x" to clear.
- **Sort button:** white circle beside the search bar. It shows "#" (by number) or an underlined
  "A" (by name). The sort options use red radio buttons and checkboxes (`figma/components/Filters.png`).
- **Pokémon card** (home grid): white rounded card with a drop shadow and a light-grey (`#EFEFEF`)
  lower half. The number (`#001`, caption, medium grey) sits top-right, the official artwork in
  the centre and the name (Body 3) at the bottom.
- **Type chip:** a pill in the type colour with white bold text (Subtitle 3).

## Screens

**Home:**
- Red background, with the Pokéball logo and "Pokédex" (Headline, white).
- The search bar and sort button below them.
- A white rounded panel holding a 3-column grid of Pokémon cards.

**Search** (by name or number): the same screen. The grid shows only the matches.

**Pokémon details:**
- Background in the colour of the first type. A big faint Pokéball watermark sits top-right.
- Header: back arrow, name (Headline, white), `#001` on the right.
- Official artwork centred, overlapping a white card. Chevrons on both sides go to the
  previous and next Pokémon.
- The card contains:
  - type chips;
  - an "About" title in the type colour;
  - a three-column row: weight (bag icon), height (ruler icon) and "Moves" (which shows
    the abilities), with grey captions below and thin dividers between them;
  - the description (Body 3);
  - a "Base Stats" title;
  - six rows with the label in the type colour (HP, ATK, DEF, SATK, SDEF, SPD), a divider,
    a 3-digit value, and a thin bar in the type colour over a light tint of it.

## CYD adaptation (320x240, landscape)

- **Fonts:** Helvetica (u8g2) stands in for Poppins.
- **Shadows:** a 1 px border replaces them.
- **Home:** shows 3x2 cards per page. Swipe up or down to change page.
- **Search:** the keyboard opens under one row of live results.
- **Details:** split into two columns. The artwork, watermark and evolutions sit on the left,
  over the type colour. The white card sits on the right.
- **Evolutions:** not in the prototype; added as tappable chips.
- **"Moves" label:** the column is called "Abilities", because that is what it shows.
