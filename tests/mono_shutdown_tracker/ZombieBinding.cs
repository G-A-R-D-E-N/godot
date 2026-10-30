using Godot;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

internal static class ZombieBinding
{
    private static readonly string MeshPath = $"user://zombie_binding_mesh_{OS.GetProcessId()}.tres";
    private static readonly string ScenePath = $"user://zombie_binding_scene_{OS.GetProcessId()}.tscn";
    private static readonly ManualResetEventSlim Entered = new();
    private static readonly ManualResetEventSlim Release = new();
    private static readonly TimeSpan GateWait = TimeSpan.FromSeconds(60);

    private sealed class Gate
    {
        ~Gate()
        {
            Entered.Set();
            Release.Wait(GateWait);
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void BlockFinalizers() => _ = new Gate();

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
    }

    private static IntPtr AssociatedHandle(IntPtr native)
    {
        var method = typeof(GodotObject).Assembly.GetType("Godot.NativeInterop.NativeFuncs")?.GetMethod(
            "godotsharp_internal_object_get_associated_gchandle", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static
        ) ?? throw new Exception("Associated handle query unavailable.");
        return (IntPtr)method.Invoke(null, new object[] { native })!;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static (WeakReference Short, WeakReference Long, IntPtr Native, ulong Id) Prepare()
    {
        var mesh = new BoxMesh();
        Check(ResourceSaver.Save(mesh, MeshPath) == Error.Ok, "The mesh was not saved.");
        mesh.TakeOverPath(MeshPath);
        var node = new MeshInstance3D { Mesh = mesh };
        using (var packed = new PackedScene())
        {
            Check(packed.Pack(node) == Error.Ok, "The scene was not packed.");
            Check(ResourceSaver.Save(packed, ScenePath) == Error.Ok, "The scene was not saved.");
        }
        node.Free();
        return (new WeakReference(mesh, false), new WeakReference(mesh, true), mesh.NativeInstance, mesh.GetInstanceId());
    }

    private static (WeakReference Pending, IntPtr Native, ulong Id, PackedScene Scene) ReloadAfterCollection()
    {
        BlockFinalizers();
        GC.Collect();
        Check(Entered.Wait(GateWait), "The finalizer gate did not start.");
        var (weak, pending, native, id) = Prepare();
        GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, true, true);
        Check(!weak.IsAlive && pending.IsAlive, "Expected a cleared short reference and a pending mesh wrapper.");
        Check(AssociatedHandle(native) != IntPtr.Zero, "Expected the mesh binding to keep its weak handle before the reload.");
        var scene = ResourceLoader.Load<PackedScene>(ScenePath, null, ResourceLoader.CacheMode.Ignore);
        Check(scene != null, "The scene did not load.");
        Check(AssociatedHandle(native) == IntPtr.Zero, "Expected the reload to find the mesh wrapper collected.");
        return (pending, native, id, scene!);
    }

    private static void Finish()
    {
        Release.Set();
        DirAccess.RemoveAbsolute(MeshPath);
        DirAccess.RemoveAbsolute(ScenePath);
    }

    private static void RunFinalizers()
    {
        Release.Set();
        GC.WaitForPendingFinalizers();
        GC.Collect();
        GC.WaitForPendingFinalizers();
    }

    internal static void Run()
    {
        try
        {
            var (_, _, id, scene) = ReloadAfterCollection();
            RunFinalizers();
            GD.Print("READY zombie binding set up: the scene reload referenced a mesh whose wrapper was collected.");
            scene.Dispose();
            Check(!GodotObject.IsInstanceIdValid(id), "Expected the scene to free the mesh.");
            GD.Print("PASS zombie: the scene released the last reference to that mesh.");
        }
        finally
        {
            Finish();
        }
    }

    internal static void RunRewrap()
    {
        try
        {
            var (pending, native, id, scene) = ReloadAfterCollection();
            var second = GodotObject.InstanceFromId(id) as RefCounted;
            Check(second != null && second.NativeInstance == native, "Expected a new wrapper for the mesh.");
            int references = second!.GetReferenceCount();
            var handle = AssociatedHandle(native);
            Check(handle != IntPtr.Zero && GCHandle.FromIntPtr(handle).Target == second, "Expected the binding to hold the new wrapper's handle.");
            RunFinalizers();
            Check(!pending.IsAlive, "Expected the old wrapper to be finalized.");
            Check(GodotObject.IsInstanceValid(second) && GodotObject.IsInstanceIdValid(id), "Expected the new wrapper and the mesh to stay valid after the old wrapper was finalized.");
            Check(second.GetReferenceCount() == references - 1, "Expected the old wrapper's finalizer to drop its one reference.");
            Check(AssociatedHandle(native) == handle && GCHandle.FromIntPtr(handle).Target == second, "Expected the binding to keep the new wrapper's handle.");
            GD.Print("READY zombie rewrap set up: the old wrapper was finalized while a new wrapper held the mesh.");
            second.Dispose();
            scene.Dispose();
            Check(!GodotObject.IsInstanceIdValid(id), "Expected the scene to free the mesh.");
            GD.Print("PASS zombie-rewrap: the new wrapper stayed valid and the mesh was freed.");
        }
        finally
        {
            Finish();
        }
    }
}
