# DEADC0DE Generator

[Dead Code Overview](preservaction-deadc0de.md)

## Let's Generate!

We were promised a deadcode generator, but where is it? The designer who worked on the packaging seems to have been a bit too hasty: It seems like it never made it into the final Action Replay MK3 for the SNES. But hey, if there's no generator, we'll just build our own!

Well, how do we go about this? ‘Generator' is probably a bit of an exaggeration. This thing wouldn't know all the games, and it can't predict the future either. Let's agree on ‘Configurator' or ‘Toolkit.' Yes, we'll put together a toolkit! It will be a bit like crafting: we'll take individual snippets and stick them together. That could work.

## Snippets

### Save & Restore

Hex code for the sequence of instructions to save and restore the Processor Status, Accumulator, X, and Y Registers:

```hex
0848DA5A
xxxxxxxx
xxxxxxxx
xxxxxxxx
xxxxxxxx
6828FA7A
```

> XXXXXXXX can be replaced by your dead code
