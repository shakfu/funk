# Named arguments

Status: design agreed 2026-11-01. Not implemented or prototyped.

A signature may name parameters and give them defaults. `.{ }` then fills them by name.

```haskell
adsr :: attack:Sig = 0.1
     -> decay:Sig = 0.5
     -> sustain:Sig = 0.3
     -> release:Sig = 0.3
     -> gate:Sig
     -> Sig
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
- **Position:** `.{` follows a known name directly, and nothing else. `adsr 0.1 .{ decay = 1 }` is a compile error. A local that shadows the name is not the known function, so `\adsr -> adsr .{ }` is a compile error.
- **Signatures:** a named signature may take one parameter per line.
- **Names:** a signature's names are the API; parameter names stay local. `attack:Sig` over a parameter `a` is valid, and renaming `a` breaks no caller.
- **Partial naming:** a signature may name some parameters. The others stay positional.
- **Which functions:** top-level functions and foreign imports with named signatures. Class methods are excluded. `.{ }` on anything else, such as a function parameter, is a compile error.
- **Purity:** every value in a named-args operation is pure: a default, a preset's field, and a field in a call. Any other value is a compile error, and is bound first: `k <- readKnob ()`, then `adsr .{ attack = k }`. The called function may still have effects. A default or a preset's field cannot refer to parameters.
- **Evaluation:** field values run once. In an unfilled call they run when `.{ }` is evaluated, not at each later call, so `adsr .{ attack = f x }` computes `f x` once. If two values fail, which error is raised is not specified.
- **Effects:** `.{ }` keeps each `->` or `->>` as declared.
- **Literals:** a field takes its type from the signature, so `attack = 0.2` can be a `Sig`.
- **Presets:** a top-level binding `p = f .{ ... }` keeps `f`'s names, and its fields become `p`'s defaults. Its positional parameters are those with no value, and its own fields can still be overridden by name. A field may refer to top-level values. The form is exact: `pluck g = adsr .{ ... } g` is an ordinary function, and `pluck .{ }` on it is an error saying that only `p = f .{ ... }` keeps names.
- **Errors:** an unknown name, a name given twice, and a positional-only target are compile errors. `adsr { attack = 0.2 }` on a function with names is an error suggesting `.{`.

## Implementation

`.{ }` is rewritten to a positional call before inference. A call with every argument becomes a direct call, so it allocates nothing and works in kernels. Otherwise it becomes a lambda, which kernels already reject. Purity lets the rewrite pass values in declared order. An unfilled call binds each value that is not a literal or a variable outside the lambda, strictly: an ordinary `let` can be lazy.

- **Parser:** `name:Type = default` in a signature, and `.{` as an adjacent `.` and `{`. Checked 2026-11-01: `f :: x:Int -> Int` and `f . { }` are both parse errors today, so neither form changes the meaning of a valid program.
- **Globals:** each stores its names and defaults, across modules, so `d.adsr .{ }` resolves.
- **Check:** the rewrite, before inference.
- **Presets:** a preset's global stores its target and one hidden top-level value per field. `pluck .{ release = 1 } gate` becomes `adsr $pluck.attack 0.5 $pluck.sustain 1 gate`. Copying the field's expression to each use was the alternative: a local at the use could shadow its names, and it would recompute at every use.

## Decisions

- **Defaults in the signature, over the definition (`adsr a=0.1 ... = ...`).** Several `=` in a function head are hard to parse, multi-clause definitions have no place for them, and foreign imports have no definition.
- **A rewrite, over a generated record type.** Generated fields would compete in field inference, which picks the only loaded type with a field, so `e.attack` could become ambiguous. An unnamed type also cannot be written in a signature.
- **Strict purity, over allowing effects in values.** Effects would need an order for fields written out of declared order, and moving a value between a default, a preset and a call could change when it runs. The cost is one extra binding before a call with an effectful value.
- **`.{`, over reusing `f { }`.** A function with names cannot also be a record, so the dot is not strictly needed. It keeps a call visibly apart from a record update.
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

This is likely to be needed, but it is not part of the first version, and it needs its own design:

- **Arity:** `pluck` is a `Sig -> Sig` where a `Sig -> Sig -> Sig` is expected, so the adapter must run before unification rejects the call. The checker must know the parameter's type when it reaches the argument, which function calls do not need today.
- **Unknown arguments:** an argument that is not a known function, such as a parameter passed on, has no names to adapt from. It probably has to be a compile error unless its own type is named.

Add it when wrappers like the lambda above recur.

## Build order

Each stage is useful alone.

1. Named signatures, defaults and the rewrite, on `lib/dsp.fk`'s 8 functions of four or more parameters: `adsr`, `adsrExp`, `compress`, `comb`, `allpass`, `chorus`, `flanger`, `reverb`.
2. Preset inheritance, once patches use presets.
3. Named parameter types, once wrappers justify the design work.
