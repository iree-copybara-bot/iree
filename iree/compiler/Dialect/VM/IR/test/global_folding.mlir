// Tests folding and canonicalization of global ops.

// RUN: iree-opt -split-input-file -pass-pipeline='vm.module(canonicalize)' %s | IreeFileCheck %s

// CHECK-LABEL: @global_i32_folds
vm.module @global_i32_folds {
  // CHECK: vm.global.i32 @g0 mutable 123 : i32
  vm.global.i32 @g0 mutable init(@g0init) : i32
  vm.func @g0init() -> i32 {
    %c123 = vm.const.i32 123 : i32
    vm.return %c123 : i32
  }

  // CHECK: vm.global.i32 @g1 mutable : i32
  vm.global.i32 @g1 mutable 0 : i32
  // CHECK: vm.global.i32 @g2 : i32
  vm.global.i32 @g2 0 : i32

  // CHECK: vm.global.i32 @g3 mutable : i32
  vm.global.i32 @g3 mutable init(@g3init) : i32
  vm.func @g3init() -> i32 {
    %c0 = vm.const.i32 0 : i32
    vm.return %c0 : i32
  }
}

// -----

// CHECK-LABEL: @global_ref_folds_null
vm.module @global_ref_folds_null {
  // CHECK: vm.global.ref @g0 mutable : !iree.opaque_ref
  vm.global.ref @g0 mutable init(@g0init) : !iree.opaque_ref
  vm.func @g0init() -> !iree.opaque_ref {
    %null = vm.const.ref.zero : !iree.opaque_ref
    vm.return %null : !iree.opaque_ref
  }
}

// -----

// CHECK-LABEL: @global_load_i32_folds
vm.module @global_load_i32_folds {
  vm.global.i32 @g0 123 : i32
  // CHECK-LABEL: @inline_const_value
  vm.func @inline_const_value() -> i32 {
    // CHECK-NEXT: %c123 = vm.const.i32 123 : i32
    // CHECK-NEXT: vm.return %c123 : i32
    %g0 = vm.global.load.i32 @g0 : i32
    vm.return %g0 : i32
  }

  vm.global.i32 @g1 mutable 123 : i32
  // CHECK-LABEL: @ignore_nonconst_value
  vm.func @ignore_nonconst_value() -> i32 {
    // NOTE: ensure we don't inline non-constant values.
    // CHECK-NEXT: %g1 = vm.global.load.i32 @g1 : i32
    // CHECK-NEXT: vm.return %g1 : i32
    %g1 = vm.global.load.i32 @g1 : i32
    vm.return %g1 : i32
  }
}

// -----

// CHECK-LABEL: @global_load_ref_folds
vm.module @global_load_ref_folds {
  vm.global.ref @g0 : !iree.opaque_ref
  // CHECK-LABEL: @inline_const_null
  vm.func @inline_const_null() -> !iree.opaque_ref {
    // CHECK-NEXT: %null = vm.const.ref.zero : !iree.opaque_ref
    // CHECK-NEXT: vm.return %null : !iree.opaque_ref
    %g0 = vm.global.load.ref @g0 : !iree.opaque_ref
    vm.return %g0 : !iree.opaque_ref
  }

  vm.global.ref @g1 mutable : !iree.opaque_ref
  // CHECK-LABEL: @ignore_nonconst_value
  vm.func @ignore_nonconst_value() -> !iree.opaque_ref {
    // NOTE: ensure we don't inline non-constant values.
    // CHECK-NEXT: %g1 = vm.global.load.ref @g1 : !iree.opaque_ref
    // CHECK-NEXT: vm.return %g1 : !iree.opaque_ref
    %g1 = vm.global.load.ref @g1 : !iree.opaque_ref
    vm.return %g1 : !iree.opaque_ref
  }
}
