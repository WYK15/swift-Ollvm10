// RUN: rm -rf %t
// RUN: %clang_cc1 %s -fsyntax-only -fmodules -fimplicit-module-maps -fmodules-cache-path=%t -F%S/Inputs/interface-diagnose-missing-import -verify
@interface NSObject
@end
@interface Buggy : NSObject
@end

@import Foo.Bar;

@interface Buggy (MyExt) // expected-error {{definition of 'Buggy' must be imported from module 'Foo' before it is required}}
@end

// expected-note@Foo/RandoPriv.h:3{{previous definition is here}}
