load(
    "@tsl//tsl/platform/default:cuda_build_defs.bzl",
    "if_cuda_is_configured",
)

load(
    "@rules_pkg//pkg:tar.bzl",
    "pkg_tar",
)

cc_binary(
    name = "_XLAC.so",
    copts = [
        "-DTORCH_API_INCLUDE_EXTENSION_H",
        "-DTORCH_EXTENSION_NAME=_XLAC",
        "-fopenmp",
        "-fPIC",
        "-fwrapv",
    ],
    linkopts = [
        "-Wl,-rpath,$$ORIGIN/torch_xla/lib",  # for libtpu
        "-Wl,-soname,_XLAC.so",
        "-lstdc++fs",  # For std::filesystem
    ],
    linkshared = 1,
    visibility = ["//visibility:public"],
    deps = [
        "//torch_xla/csrc:init_python_bindings",
        "@torch//:headers",
        "@torch//:libc10",
        "@torch//:libtorch",
        "@torch//:libtorch_cpu",
        "@torch//:libtorch_python",
    ] + if_cuda_is_configured([
        "@xla//xla/stream_executor:cuda_platform",
    ]),
)

test_suite(
    name = "cpp_tests",
    # testonly = True,
    tests = [
        "//test/cpp:test_aten_xla_tensor_1",
        "//test/cpp:test_aten_xla_tensor_2",
        "//test/cpp:test_aten_xla_tensor_3",
        "//test/cpp:test_aten_xla_tensor_4",
        "//test/cpp:test_aten_xla_tensor_5",
        "//test/cpp:test_aten_xla_tensor_6",
        "//torch_xla/csrc/runtime:pjrt_computation_client_test",
        "//torch_xla/csrc/runtime:ifrt_computation_client_test",
        "//test/cpp:test_ir",
        "//test/cpp:test_lazy",
        "//test/cpp:test_replication",
        "//test/cpp:test_tensor",
        "//test/cpp:test_xla_sharding",
    ],
)

pkg_tar(
    name = "cpp_tests_tar",
    testonly = True,
    srcs = [
        ":cpp_tests"
    ]
)
