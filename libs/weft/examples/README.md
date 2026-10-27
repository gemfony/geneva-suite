# Weft examples

Small, self-contained programs that show how to use **Weft**, the standalone
serialization engine. Each links only against `gemfony-weft` and Boost.JSON — no
other Geneva library — so they double as proof that Weft is reusable on its own.

| Example | Shows |
|---|---|
| `quickstart.cpp` | The minimal case: a value type with an intrusive member `serialize`, round-tripped through the **binary** and **JSON** codecs. |
| `nonintrusive.cpp` | Serializing a **third-party type you cannot modify**, via a free `gem_archive_serialize(Archive&, T&)` found by argument-dependent lookup in the type's namespace. Nested containers compose automatically. |
| `polymorphic.cpp` | Round-tripping a `shared_ptr` to a hierarchy root and **reconstructing the exact dynamic type**, using one `GEM_REGISTER_ARCHIVABLE(Type)` per concrete class (the analogue of `BOOST_CLASS_EXPORT`), plus the boot-time completeness check. |

## Building and running

The examples are built as part of the normal Geneva build when
`GENEVA_BUILD_EXAMPLES=TRUE`. The resulting binaries are `weft_quickstart`,
`weft_nonintrusive` and `weft_polymorphic`; each prints what it round-tripped and
asserts that the reconstructed value matches the original.

```console
$ ./weft_quickstart
binary: round-tripped 24 bytes
json:   {"id":42,"mass":"0x1.7cf5c28f5c28fp-101","label":"electron"}
quickstart: OK
```

## The serialize contract in one paragraph

A type opts in exactly as it would with Boost.Serialization: a member
`template <typename Archive> void serialize(Archive& ar, unsigned version)` (make
it private and `friend struct Gem::Weft::access;` to keep it encapsulated), or a
free `gem_archive_serialize(Archive&, T&)` in the type's namespace when the class
can't be touched. Inside, stream each field with `ar & Gem::Weft::make_nvp("name",
field)`; for a base slice use `ar & Gem::Weft::base_object<Base>(*this)`. The same
method serves every codec.
