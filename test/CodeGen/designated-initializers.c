// RUN: %clang_cc1 -triple i386-unknown-unknown %s -emit-llvm -o - | FileCheck %s

struct foo {
    void *a;
    int b;
};

// CHECK: @u = global %union.anon zeroinitializer
union { int i; float f; } u = { };

// CHECK: @u2 = global { i32, [4 x i8] } { i32 0, [4 x i8] undef }
union { int i; double f; } u2 = { };

// CHECK: @u3 = global  %union.anon.1 zeroinitializer
union { double f; int i; } u3 = { };

// CHECK: @b = global [2 x i32] [i32 0, i32 22]
int b[2] = {
  [1] = 22
};

// PR6955

struct ds {
  struct {
    struct {
      short a;
    };
    short b;
    struct {
      short c;
    };
  };
};

// Traditional C anonymous member init
struct ds ds0 = { { { .a = 0 } } };
// C1X lookup-based anonymous member init cases
struct ds ds1 = { { .a = 1 } };
struct ds ds2 = { { .b = 1 } };
struct ds ds3 = { .a = 0 };
// CHECK: @ds4 = global %struct.ds { %struct.anon.3 { %struct.anon zeroinitializer, i16 0, %struct.anon.2 { i16 1 } } }
struct ds ds4 = { .c = 1 };
struct ds ds5 = { { { .a = 0 } }, .b = 1 };
struct ds ds6 = { { .a = 0, .b = 1 } };
// CHECK: @ds7 = global %struct.ds { %struct.anon.3 { %struct.anon { i16 2 }, i16 3, %struct.anon.2 zeroinitializer } }
struct ds ds7 = {
  { {
      .a = 1
    } },
  .a = 2,
  .b = 3
};


// <rdar://problem/10465114>
struct overwrite_string_struct1 {
  __typeof(L"foo"[0]) L[6];
  int M;
} overwrite_string1[] = { { { L"foo" }, 1 }, [0].L[2] = L'x'};
// CHECK: [6 x i32] [i32 102, i32 111, i32 120, i32 0, i32 0, i32 0], i32 1
struct overwrite_string_struct2 {
  char L[6];
  int M;
} overwrite_string2[] = { { { "foo" }, 1 }, [0].L[2] = 'x'};
// CHECK: [6 x i8] c"fox\00\00\00", i32 1
struct overwrite_string_struct3 {
  char L[3];
  int M;
} overwrite_string3[] = { { { "foo" }, 1 }, [0].L[2] = 'x'};
// CHECK: [3 x i8] c"fox", i32 1
struct overwrite_string_struct4 {
  char L[3];
  int M;
} overwrite_string4[] = { { { "foobar" }, 1 }, [0].L[2] = 'x'};
// CHECK: [3 x i8] c"fox", i32 1
struct overwrite_string_struct5 {
  char L[6];
  int M;
} overwrite_string5[] = { { { "foo" }, 1 }, [0].L[4] = 'y'};
// CHECK: [6 x i8] c"foo\00y\00", i32 1



void test1(int argc, char **argv)
{
  // CHECK: internal global %struct.foo { i8* null, i32 1024 }
  static struct foo foo = {
    .b = 1024,
  };

  // CHECK: bitcast %union.anon.4* %u2
  // CHECK: call void @llvm.memset
   union { int i; float f; } u2 = { };

  // CHECK-NOT: call void @llvm.memset
  union { int i; float f; } u3;

  // CHECK: ret void
}


// PR7151
struct S {
  int nkeys;
  int *keys;
  union {
    void *data;
  };
};

void test2() {
  struct S *btkr;
  
  *btkr = (struct S) {
    .keys  = 0,
    { .data  = 0 },
  };
}
