// Test whether or not the driver instructs the backend to use .init_array
// sections for global constructors.
//
// CHECK-INIT-ARRAY: -fuse-init-array
// CHECK-NO-INIT-ARRAY-NOT: -fuse-init-array
//
// RUN: %clang -no-canonical-prefixes %s -### -fsyntax-only 2>&1       \
// RUN:     -target i386-unknown-linux \
// RUN:     --sysroot=%S/Inputs/fake_install_tree \
// RUN:   | FileCheck --check-prefix=CHECK-INIT-ARRAY %s
//
// RUN: %clang -no-canonical-prefixes %s -### -fsyntax-only 2>&1       \
// RUN:     -fno-use-init-array \
// RUN:     -target i386-unknown-linux \
// RUN:     --sysroot=%S/Inputs/fake_install_tree \
// RUN:   | FileCheck --check-prefix=CHECK-NO-INIT-ARRAY %s
//
// RUN: %clang -no-canonical-prefixes %s -### -fsyntax-only 2>&1       \
// RUN:     -fno-use-init-array -fuse-init-array \
// RUN:     -target i386-unknown-linux \
// RUN:     --sysroot=%S/Inputs/fake_install_tree \
// RUN:   | FileCheck --check-prefix=CHECK-INIT-ARRAY %s
//
// RUN: %clang -no-canonical-prefixes %s -### -fsyntax-only 2>&1       \
// RUN:     -target i386-unknown-linux \
// RUN:     --sysroot=%S/Inputs/basic_linux_tree \
// RUN:   | FileCheck --check-prefix=CHECK-NO-INIT-ARRAY %s
//
// RUN: %clang -no-canonical-prefixes %s -### -fsyntax-only 2>&1       \
// RUN:     -fuse-init-array \
// RUN:     -target i386-unknown-linux \
// RUN:     --sysroot=%S/Inputs/basic_linux_tree \
// RUN:   | FileCheck --check-prefix=CHECK-INIT-ARRAY %s
//
// RUN: %clang -no-canonical-prefixes %s -### -fsyntax-only 2>&1       \
// RUN:     -target arm-unknown-linux-androideabi \
// RUN:     --sysroot=%S/Inputs/basic_android_tree/sysroot \
// RUN:   | FileCheck --check-prefix=CHECK-INIT-ARRAY %s
//
// RUN: %clang -no-canonical-prefixes %s -### -fsyntax-only 2>&1       \
// RUN:     -target mipsel-unknown-linux-android \
// RUN:     --sysroot=%S/Inputs/basic_android_tree/sysroot \
// RUN:   | FileCheck --check-prefix=CHECK-INIT-ARRAY %s
//
// RUN: %clang -no-canonical-prefixes %s -### -fsyntax-only 2>&1       \
// RUN:     -target i386-unknown-linux-android \
// RUN:     --sysroot=%S/Inputs/basic_android_tree/sysroot \
// RUN:   | FileCheck --check-prefix=CHECK-INIT-ARRAY %s
