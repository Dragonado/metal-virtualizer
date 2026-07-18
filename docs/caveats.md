# Caveats

## Cached Foundation strings

`NS::String::string(...)` returns an autoreleased object. If `Device` stores
that pointer in a member, its constructor should retain it; otherwise it may
disappear when an autorelease pool drains.
