# Diablo 2 Seed Finder & Map Renderer

Diablo 2 only uses a 31-bit seed for the map generation RNG; this can be brute-forced and reversed.

See it in action: [map renderer](https://maps.diablo.deadlybossmods.com) and [seed finder](https://seeds.diablo.deadlybossmods.com).

**Requirements**: Diablo 2 version 1.13c, cmake, and node.

The code is 99% AI-generated and some parts are not great.
I won't insult you with an AI-generated readme here, but I also don't feel like writing one myself, so, sorry, no documentation.
Probably best to just ask your AI how this works and how to run it.


# Using the seed finder

Due to compute requirements the seed finder currently requires a full reconstruction of tower level 4 on Normal.
With some compute it could be simplified into a 20-questions-style thing asking for more obviously visible things like exit directions, sprites used for various objects, etc.

Why tower specifically? The graves!
Take a look at this screenshot: [img/graves.png](img/graves.png).
See the 4 closed and 2 open graves?
This particular dead end room will always have exactly 6 of these in the exact same positions. But whether they are open or closed is random and yields about a bit of information about the RNG seed.

To use it you'll need to understand how random maps are generated: they are built from a few pre-defined "rooms" that are all the same size. You'll need to learn to spot and identify these in the game (which is rather easy with a bit of practice).

The flow is roughly:

1. Which directions does it connect in? There are corridors, corners, intersections and dead ends. The *connection points are never doors, always wide open areas*.
2. Which variant of a room is it? Most rooms have 3 variants; two are mostly empty/have no special elements and one has a few special elements. For example, the bottom corner piece can have a burning dead rogue inside for the special variant, and the two empty variants differ by which side the door is on.
3. Identify the positions the graves are in; the tool shows this on a map. These always refer to non-interactable graves on the floor that can be walked over when closed or are non-walkable when open. Graves that can be clicked on or are caskets that are above the ground are not relevant for the tool and must be ignored.
4. Once you've placed enough rooms in the given order to narrow it down to fewer than 200 (configurable) possible seeds, the web app will offer the option to brute-force the remaining seeds. Takes a few seconds of compute on my server, might be overloaded.

Internally it's just a large trie storing the level in BFS order followed by some ad-hoc brute-forcing.