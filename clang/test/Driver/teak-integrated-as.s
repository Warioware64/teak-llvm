// Teak has no external assembler: clang must use the integrated one by
// default, for .s files and for .S / -x assembler-with-cpp (#define, #include).

// RUN: %clang -### --target=teak -c %s 2>&1 | FileCheck %s --check-prefix=AS
// AS: "-cc1as"
// AS-NOT: "{{[^"]*}}/as"

// RUN: %clang -### --target=teak -c -x assembler-with-cpp %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CPP
// CPP: "-cc1" {{.*}}"-E"
// CPP: "-cc1as"
// CPP-NOT: "{{[^"]*}}/as"

// RUN: %clang -### --target=teak -c -x c /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=C
// C: "-cc1" {{.*}}"-emit-obj"
// C-NOT: "{{[^"]*}}/as"
