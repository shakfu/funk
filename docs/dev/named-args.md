# Named arguments

Status: design agreed 2026-11-01. Not implemented or prototyped.

A signature may name parameters and give them defaults. `.{ }` then fills them by name.

```haskell
adsr :: attack:Sig = 0.1 -> decay:Sig = 0.5 -> sustain:Sig = 0.3 -> release:Sig = 0.3 -> gate:Sig -> Sig
adsr a d s r gate = ...

adsr 0.1                      -- partial application: 4 arguments still to come
adsr .{ attack = 0.2 }        -- gate:Sig -> Sig
adsr .{ attack = 0.2 } gate   -- Sig
adsr .{ gate = g }            -- Sig
adsr .{ }                     -- gate:Sig -> Sig

pluck = adsr .{ attack = 0.001, sustain = 0 }   -- a preset: Sig -> Sig
pluck gate                                      -- positional
pluck .{ release = 1 } gate                     -- adsr 0.001 0.5 0 1 gate
soft = pluck .{ release = 1 }                   -- presets chain
```

## Rules

- **Currying:** positional calls never use defaults, and the function's type stays a chain of arrows. Existing code is unaffected.
- **`.{ }`:** fills the named fields and the defaults. Parameters with no value stay positional, in declared order.
- **Names:** a signature's names are the API; parameter names stay local. `attack:Sig` over a parameter `a` is valid, and renaming `a` breaks no caller.
- **Partial naming:** a signature may name some parameters. The others stay positional.
- **Which functions:** top-level functions and foreign imports with named signatures. Class methods are excluded. `.{ }` on anything else, such as a function parameter, is a compile error.
- **Defaults:** pure, and cannot refer to other parameters. Evaluated at each call that uses them.
- **Evaluation order:** field values run as written, then the call runs in declared order, since evaluation is strict and effects must not reorder.
- **Effects:** `.{ }` keeps each `->` or `->>` as declared.
- **Literals:** a field takes its type from the signature, so `attack = 0.2` can be a `Sig`.
- **Presets:** a top-level binding `p = f .{ ... }` keeps `f`'s names, and its fields become `p`'s defaults. Its positional parameters are those with no value, and its own fields can still be overridden by name. Each field value is evaluated once, when the preset is defined, so unlike a signature's default it may have effects and may refer to any top-level value.
- **Errors:** an unknown name, a name given twice, and a positional-only target are compile errors.

## Implementation

`.{ }` is rewritten to a positional call before inference. A call with every argument becomes a direct call, so it allocates nothing and works in kernels. Otherwise it becomes a lambda, which kernels already reject.

- **Parser:** `name:Type = default` in a signature, and `.{` as an adjacent `.` and `{`. Checked 2026-11-01: `f :: x:Int -> Int` and `f . { }` are both parse errors today, so neither form changes the meaning of a valid program.
- **Globals:** each stores its names and defaults, across modules, so `d.adsr .{ }` resolves.
- **Check:** the rewrite, before inference.
- **Presets:** a preset's global stores its target and one hidden top-level value per field. `pluck .{ release = 1 } gate` becomes `adsr $pluck.attack 0.5 $pluck.sustain 1 gate`. Copying the field's expression to each use was the alternative: a local at the use could shadow its names, and it would run at every use.

## Decisions

- **Defaults in the signature, over the definition (`adsr a=0.1 ... = ...`).** Several `=` in a function head are hard to parse, multi-clause definitions have no place for them, and foreign imports have no definition.
- **A rewrite, over a generated record type.** Generated fields would compete in field inference, which picks the only loaded type with a field, so `e.attack` could become ambiguous. An unnamed type also cannot be written in a signature.
- **Over labels in types, as in OCaml.** Those survive higher-order use, but affect unification, class methods, effects and error messages.
- **Over a handwritten record per function.** That works today, but needs a type and a helper for each function.
- **Preset inheritance, over wrappers.** A wrapper needs no compiler support and can hide parameters, but restates types and defaults. Wrappers still work where a preset should hide a parameter.

## Limits

- A function passed as a value loses its names, because they are on the global, not in its type. `applyEnv f = f .{ release = 1 } 1` is an error. A caller can override first, `applyEnv (pluck .{ release = 1 })`, or the receiver can take the varying value as a parameter: `spread (\r gate -> pluck .{ release = r } gate)`.
- Only top-level bindings are presets. `let p = adsr .{ attack = x } in p .{ release = 1 }` is an error, since `x` can be a local. So is overriding `if soft then adsr .{ ... } else adsr .{ ... }`, which has no single target.
- A preset's hidden value has one type, so it fixes the type of a polymorphic field it sets.
- A signature's names become public API, so renaming one breaks callers.

Not checked: after `adsr` is evaluated again in a plugin, `pluck` is assumed to call the old `adsr` until it too is evaluated again, as other top-level code does.

## Likely extension: named parameter types

A parameter's type may carry names, so a receiver can override a function it was given:

```haskell
spread :: (release:Sig -> gate:Sig -> Sig) -> Sig
spread env = voicesIx (\k gate freq -> env .{ release = toSig k * 0.1 } gate * d.saw freq)

sig = spread pluck
```

The names come from `env`'s annotation. At the call, the compiler adapts `pluck`, a `Sig -> Sig`, to the parameter's shape:

```haskell
sig = spread (\release gate -> pluck .{ release = release } gate)
```

Names stay out of unification, so this costs far less than labels in types. It is not part of the first version: a generated wrapper at each such call needs its own design for an argument that is not a known function, such as a parameter passed on. Add it when wrappers like the lambda above recur.
