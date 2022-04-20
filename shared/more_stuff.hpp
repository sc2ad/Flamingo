/*
Okay, so more stuff

We want to have all hooks be preinstalled early
Then, we take all hooks that are to be installed at a given location
And, well, we install them

We can worry about hooks that need to be installed specifically later, we just need to have hooks have a way of:
-- where to install (function call, maybe address?)
--- should resolve to an address
-- target pointer/functor (ex, contextual lambdas should be permissible, albeit hard to do early enough)
-- metadata (ex, orig hook, trampoline or no, etc.)
-- trampoline pointer to write to
-- additional asm to write?
--- somehow we should try to make transpilers possible, though that can be figured out later

Once we have all of the hooks:
- resolve all hook targets
- resolve metadata (ex, only one orig hook per target)
- hook (and optionally allocate trampoline) once per target
-- (potentially) make use of target method size to determine if optimizations need to take place/if hook cannot be made
- create innard prologue with identical signature (size of parameters matter, not types)
- forward parameters to each method call
-- calling the orig no longer is applicable due to ordering, prefix/postfix only.
-- alternatively, innard prologue only forwards to most specific hook, orig calls forward to next in chain
-- however, it must be a requirement that a given hook calls orig for this to be the case (or if it does not, fallthrough to call it on return)
- profit?
*/
