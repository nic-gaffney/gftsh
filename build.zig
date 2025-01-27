const std = @import("std");

pub fn build(b: *std.Build) !void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    var files = std.ArrayList([]const u8).init(b.allocator);
    defer {
        for (files.items) |item|
            b.allocator.free(item);
        files.deinit();
    }

    const sources: []const []const u8 = &[_][]const u8{
        "src",
    };
    const includes: []const []const u8 = &[_][]const u8{};

    for (sources) |directory|
        try getFiles(b, &files, directory);

    const shell = b.addExecutable(.{
        .optimize = optimize,
        .target = target,
        .name = "shell",
    });

    shell.linkLibC();
    for (includes) |dir|
        shell.addIncludePath(b.path(dir));
    shell.addCSourceFiles(.{
        .files = files.items,
    });
    b.installArtifact(shell);

    const run = b.addRunArtifact(shell);
    run.step.dependOn(b.getInstallStep());
    if (b.args) |arg| run.addArgs(arg);
    const runstep = b.step("run", "Run the shell");
    runstep.dependOn(&run.step);
}

fn getFiles(b: *std.Build, files: *std.ArrayList([]const u8), directory: []const u8) !void {
    var dir = try std.fs.cwd().openDir(directory, .{ .iterate = true });
    defer dir.close();
    var it = dir.iterate();
    while (try it.next()) |file| {
        const fullpath = try std.mem.concat(b.allocator, u8, &[_][]const u8{ directory, "/", file.name });
        if (file.kind == .directory) {
            try getFiles(b, files, fullpath);
            continue;
        }
        if (file.kind != .file) continue;
        if (!std.ascii.endsWithIgnoreCase(file.name, ".c")) continue;
        try files.append(fullpath);
    }
}
