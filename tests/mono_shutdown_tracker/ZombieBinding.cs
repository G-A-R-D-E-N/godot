using Godot;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

internal static class ZombieBinding
{
    private const string MeshPath = "user://zombie_binding_mesh.tres";
    private const string ScenePath = "user://zombie_binding_scene.tscn";
    private static readonly ManualResetEventSlim Entered = new();
    private static readonly ManualResetEventSlim Release = new();

    private sealed class Gate
    {
        ~Gate()
        {
            Entered.Set();
            Release.Wait(TimeSpan.FromSeconds(15));
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

    internal static void Run()
    {
        try
        {
            BlockFinalizers();
            GC.Collect();
            Check(Entered.Wait(TimeSpan.FromSeconds(5)), "The finalizer gate did not start.");
            var (weak, pending, native, id) = Prepare();
            GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, true, true);
            Check(!weak.IsAlive && pending.IsAlive, "Expected a cleared short reference and a pending mesh wrapper.");
            Check(AssociatedHandle(native) != IntPtr.Zero, "Expected the mesh binding to keep its weak handle before the reload.");
            var scene = ResourceLoader.Load<PackedScene>(ScenePath, null, ResourceLoader.CacheMode.Ignore);
            Check(scene != null, "The scene did not load.");
            Check(AssociatedHandle(native) == IntPtr.Zero, "Expected the reload to find the mesh wrapper collected.");
            Release.Set();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GD.Print("READY zombie binding set up: the scene reload referenced a mesh whose wrapper was collected.");
            scene!.Dispose();
            Check(!GodotObject.IsInstanceIdValid(id), "Expected the scene to free the mesh.");
            GD.Print("PASS zombie: the scene released the last reference to that mesh.");
        }
        finally
        {
            Release.Set();
        }
    }
}
