const std = @import("std");

const common_flags = &[_][]const u8{
    "-march=x86-64",
    "-mtune=generic",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-std=c23",
    "-D_GNU_SOURCE",
};

const debug_flags = &[_][]const u8{
    "-march=x86-64",
    "-mtune=generic",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-std=c23",
    "-D_GNU_SOURCE",
    "-fsanitize=address",
    "-fsanitize=undefined",
    "-fsanitize=leak",
};

fn addServerExecutable(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    enable_sanitizers: bool,
) *std.Build.Step.Compile {
    const use_sanitizers = enable_sanitizers and optimize == .Debug;
    const c_flags = if (use_sanitizers) debug_flags else common_flags;

    const exe = b.addExecutable(.{
        .name = "jsonrpc_server",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });

    // Add C source files
    exe.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &.{
            "main.c",
            "server.c",
            "jsonrpc.c",
            "parson.c",
        },
        .flags = c_flags,
    });

    // Include the current directory for headers
    exe.addIncludePath(b.path("src"));

    // Link against system libuv
    // This requires libuv headers and libraries to be in standard system paths.
    exe.linkSystemLibrary("uv");

    // Link against the C standard library
    exe.linkLibC();

    if (use_sanitizers) {
        exe.bundle_compiler_rt = true;
        exe.bundle_ubsan_rt = true;
    }

    return exe;
}

pub fn build(b: *std.Build) void {
    // Standard target options (arch, os, abi). We rely on C flags below to pin
    // the ISA to a Valgrind-friendly baseline without forcing a cross target.
    const target = b.standardTargetOptions(.{});

    // Standard optimization options (Debug, ReleaseSafe, ReleaseFast, ReleaseSmall)
    const optimize = b.standardOptimizeOption(.{});

    const enable_sanitizers = b.option(bool, "sanitizers", "Enable ASan/UBSan/LSan in Debug builds.") orelse false;
    const exe = addServerExecutable(b, target, optimize, enable_sanitizers);

    // If you add crypto for handshakes (e.g., OpenSSL), link it here:
    // exe.linkSystemLibrary("ssl");
    // exe.linkSystemLibrary("crypto");

    // Install the artifact (moves it to zig-out/bin)
    b.installArtifact(exe);

    const debug_exe = addServerExecutable(b, target, .Debug, enable_sanitizers);
    const debug_install = b.addInstallArtifact(debug_exe, .{});
    const debug_step = b.step("debug", "Build Debug");
    debug_step.dependOn(&debug_install.step);

    const release_exe = addServerExecutable(b, target, .ReleaseFast, enable_sanitizers);
    const release_install = b.addInstallArtifact(release_exe, .{});
    const release_step = b.step("release", "Build ReleaseFast");
    release_step.dependOn(&release_install.step);

    // Create a 'run' step to execute the server directly via 'zig build run'
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the JSON-RPC server");
    run_step.dependOn(&run_cmd.step);
}
