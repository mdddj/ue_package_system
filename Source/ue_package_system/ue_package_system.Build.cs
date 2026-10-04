using System;
using System.IO;
using UnrealBuildTool;

public class ue_package_system : ModuleRules
{
    public ue_package_system(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "UMG",   // 公开头文件里的控件类派生自 UUserWidget（UMG 模块），下游包含我们的头也需要它
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Projects",
            "Slate",
            "SlateCore",
        });

        // ================= Rust 桥接 =================
        //
        // Rust crate 在 Source/ue_package_system-dylib，头文件由 cbindgen 在 cargo build
        // 时通过 build.rs 生成到它的 include/ 目录下。

        string PluginDirectory = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
        string RustDirectory = Path.Combine(ModuleDirectory, "..", "ue_package_system-dylib");

        // 只需要 include 路径，不需要链接 Rust 库：
        // 所有符号都在运行时由 FPlatformProcess::GetDllHandle + GetDllExport 取，
        // 这样 Windows 不需要额外的 .dll.lib import 库，三平台走同一条代码路径。
        PublicIncludePaths.Add(Path.GetFullPath(Path.Combine(RustDirectory, "include")));

        // 库文件由 `uerust build <PluginDir>` 拷到 Binaries/<Platform>/。
        // 这里声明它必须被 staging 进打包结果，并且是 NonUFS ——
        // dylib/dll/so 不能进 .pak，运行时没法从 pak 里 dlopen。
        string RustLibraryName;
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            RustLibraryName = "ue_package_system_dylib.dll";
        }
        else if (Target.Platform == UnrealTargetPlatform.Mac)
        {
            RustLibraryName = "libue_package_system_dylib.dylib";
        }
        else
        {
            RustLibraryName = "libue_package_system_dylib.so";
        }

        string StagedLibrary = Path.Combine(PluginDirectory, "Binaries", Target.Platform.ToString(), RustLibraryName);
        if (File.Exists(StagedLibrary))
        {
            RuntimeDependencies.Add(StagedLibrary, StagedFileType.NonUFS);
        }
        else
        {
            Console.WriteLine("ue_package_system: 找不到 " + StagedLibrary + "，先跑 `uerust build \"" + PluginDirectory + "\"`");
        }
    }
}
