// Pointers are 16 bits on Teak. A structure containing a pointer and 16-bit
// values used to crash the compiler ("Type size mismatch" in the record
// layout), because clang assumed 32-bit pointers.

// RUN: %clang_cc1 -triple teak -emit-llvm -o - %s | FileCheck %s

struct item {
    const char *name;
    short a, b;
};

// CHECK: @items = {{.*}}global [2 x %struct.item]
struct item items[2] = { { "x", 1, 2 }, { "y", 3, 4 } };

// CHECK: @ptr_size = {{.*}}global i32 2
int ptr_size = sizeof(void *);

// CHECK: @item_size = {{.*}}global i32 6
int item_size = sizeof(struct item);
