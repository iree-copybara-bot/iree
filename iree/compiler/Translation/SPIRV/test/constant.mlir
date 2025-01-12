// RUN: iree-opt -xla-legalize-to-std -split-input-file -iree-index-computation -simplify-spirv-affine-exprs=false -convert-iree-to-spirv -verify-diagnostics -o - %s | IreeFileCheck %s

module {
  func @const_f32(%arg0: memref<2x3xf32>, %arg1: memref<2x3xf32>)
  attributes  {iree.executable.export, iree.executable.workload = dense<[2, 3]> : tensor<2xi32>, iree.executable.workgroup_size = dense<[32, 1, 1]> : tensor<3xi32>, iree.ordinal = 0 : i32} {
    // CHECK: [[NUM0:%.*]] = spv.Load "StorageBuffer" {{%.*}} : f32
    // CHECK: [[CONST1:%.*]] = spv.constant dense<{{\[}}[1.000000e+00, 2.000000e+00, 3.000000e+00], [4.000000e+00, 5.000000e+00, 6.000000e+00]]> : tensor<2x3xf32> : !spv.array<2 x !spv.array<3 x f32 [4]> [12]>
    // CHECK-NEXT: [[VAR1:%.*]] = spv.Variable init([[CONST1]]) : !spv.ptr<!spv.array<2 x !spv.array<3 x f32 [4]> [12]>, Function>
    // CHECK: [[NUM1PTR:%.*]] = spv.AccessChain [[VAR1]][{{.*}}]
    // CHECK-NEXT: [[NUM1:%.*]] = spv.Load "Function" [[NUM1PTR]] : f32
    // CHECK: {{.*}} = spv.FAdd [[NUM0]], [[NUM1]] : f32
    %0 = iree.load_input(%arg0 : memref<2x3xf32>) : tensor<2x3xf32>
    %1 = "xla_hlo.constant"() {value = dense<[[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]> : tensor<2x3xf32>} : () -> (tensor<2x3xf32>)
    %2 = "xla_hlo.add"(%0, %1) : (tensor<2x3xf32>, tensor<2x3xf32>) -> tensor<2x3xf32>
    iree.store_output(%2 : tensor<2x3xf32>, %arg1 : memref<2x3xf32>)
    iree.return
  }
}

// -----

module {
  func @splat_const_f32(%arg0: memref<2x3xf32>, %arg1: memref<2x3xf32>)
  attributes  {iree.executable.export, iree.executable.workload = dense<[2, 3]> : tensor<2xi32>, iree.executable.workgroup_size = dense<[32, 1, 1]> : tensor<3xi32>, iree.ordinal = 0 : i32} {
    // CHECK: [[NUM0:%.*]] = spv.Load "StorageBuffer" {{%.*}} : f32
    // CHECK: [[CONST1:%.*]] = spv.constant 1.000000e+00 : f32
    // CHECK: {{.*}} = spv.FAdd [[NUM0]], [[CONST1]] : f32
    %0 = iree.load_input(%arg0 : memref<2x3xf32>) : tensor<2x3xf32>
    %1 = "xla_hlo.constant"() {value = dense<1.0> : tensor<2x3xf32>} : () -> (tensor<2x3xf32>)
    %2 = "xla_hlo.add"(%0, %1) : (tensor<2x3xf32>, tensor<2x3xf32>) -> tensor<2x3xf32>
    iree.store_output(%2 : tensor<2x3xf32>, %arg1 : memref<2x3xf32>)
    iree.return
  }
}

// -----

module {
  func @const_i32(%arg0: memref<2x3xi32>, %arg1: memref<2x3xi32>)
  attributes  {iree.executable.export, iree.executable.workload = dense<[2, 3]> : tensor<2xi32>, iree.executable.workgroup_size = dense<[32, 1, 1]> : tensor<3xi32>, iree.ordinal = 0 : i32} {
    // CHECK: [[NUM0:%.*]] = spv.Load "StorageBuffer" {{%.*}} : i32
    // CHECK: [[CONST1:%.*]] = spv.constant dense<{{\[}}[1, 2, 3], [4, 5, 6]]> : tensor<2x3xi32> : !spv.array<2 x !spv.array<3 x i32 [4]> [12]>
    // CHECK-NEXT: [[VAR1:%.*]] = spv.Variable init([[CONST1]]) : !spv.ptr<!spv.array<2 x !spv.array<3 x i32 [4]> [12]>, Function>
    // CHECK: [[NUM1PTR:%.*]] = spv.AccessChain [[VAR1]][{{.*}}]
    // CHECK-NEXT: [[NUM1:%.*]] = spv.Load "Function" [[NUM1PTR]] : i32
    // CHECK: {{.*}} = spv.IAdd [[NUM0]], [[NUM1]] : i32
    %0 = iree.load_input(%arg0 : memref<2x3xi32>) : tensor<2x3xi32>
    %1 = "xla_hlo.constant"() {value = dense<[[1, 2, 3], [4, 5, 6]]> : tensor<2x3xi32>} : () -> (tensor<2x3xi32>)
    %2 = "xla_hlo.add"(%0, %1) : (tensor<2x3xi32>, tensor<2x3xi32>) -> tensor<2x3xi32>
    iree.store_output(%2 : tensor<2x3xi32>, %arg0 : memref<2x3xi32>)
    iree.return
  }
}


// -----

module {
  // CHECK-DAG: spv.globalVariable [[GLOBALIDVAR:@.*]] built_in("GlobalInvocationId") : !spv.ptr<vector<3xi32>, Input>
  func @splat_const_i32(%arg0: memref<2x3xi32>, %arg1: memref<2x3xi32>)
  attributes  {iree.executable.export, iree.executable.workload = dense<[2, 3]> : tensor<2xi32>, iree.executable.workgroup_size = dense<[32, 1, 1]> : tensor<3xi32>, iree.ordinal = 0 : i32} {
    // CHECK: [[NUM0:%.*]] = spv.Load "StorageBuffer" {{%.*}} : i32
    // CHECK: [[CONST1:%.*]] = spv.constant 1 : i32
    // CHECK: {{.*}} = spv.IAdd [[NUM0]], [[CONST1]] : i32
    %0 = iree.load_input(%arg0 : memref<2x3xi32>) : tensor<2x3xi32>
    %1 = "xla_hlo.constant"() {value = dense<1> : tensor<2x3xi32>} : () -> (tensor<2x3xi32>)
    %2 = "xla_hlo.add"(%0, %1) : (tensor<2x3xi32>, tensor<2x3xi32>) -> tensor<2x3xi32>
    iree.store_output(%2 : tensor<2x3xi32>, %arg0 : memref<2x3xi32>)
    iree.return
  }
}
